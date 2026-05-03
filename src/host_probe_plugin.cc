// Minimal CLAP-first diagnostic plugin for DAW reconnaissance.
// It logs raw host callbacks and events without altering audio.

#include "clap/clap.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/gui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "choc/gui/choc_WebView.h"
#include "choc/text/choc_JSON.h"
#include "choc/containers/choc_Value.h"

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <shobjidl.h>
  #include <shlobj.h>
  #include <shellapi.h>
  #include <commdlg.h>
#else
  #include <dlfcn.h>
  #include <spawn.h>
  #include <sys/utsname.h>
#endif

#if !defined(_WIN32)
extern char** environ;
#endif

namespace {

constexpr const char* kClapPluginAsVst3Ext = "clap.plugin-info-as-vst3/0";
constexpr const char* kCasesFileName = "daw-host-probe-cases.json";
constexpr const char* kProbeVersion = "0.1.0";

struct ClapPluginAsVst3 {
    uint32_t(CLAP_ABI* getNumMIDIChannels)(const clap_plugin* plugin, uint32_t note_port);
    uint32_t(CLAP_ABI* supportedNoteExpressions)(const clap_plugin* plugin);
};

struct ProbeParam {
    clap_id id;
    const char* name;
    double min;
    double max;
    double def;
    clap_param_info_flags flags;
};

constexpr std::array<ProbeParam, 3> kParams { {
    { 1000, "Probe Float", 0.0, 1.0, 0.5, CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE },
    { 1001, "Probe Stepped", 0.0, 4.0, 0.0, CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED },
    { 1002, "Probe Toggle", 0.0, 1.0, 0.0, CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED },
} };

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    nullptr,
};

uint32_t paramIndex(clap_id id)
{
    for (uint32_t i = 0; i < kParams.size(); ++i) {
        if (kParams[i].id == id)
            return i;
    }
    return UINT32_MAX;
}

double beatTime(clap_beattime value)
{
    return static_cast<double>(value) / static_cast<double>(CLAP_BEATTIME_FACTOR);
}

double secTime(clap_sectime value)
{
    return static_cast<double>(value) / static_cast<double>(CLAP_SECTIME_FACTOR);
}

const char* eventTypeName(uint16_t type)
{
    switch (type) {
    case CLAP_EVENT_NOTE_ON: return "note_on";
    case CLAP_EVENT_NOTE_OFF: return "note_off";
    case CLAP_EVENT_NOTE_CHOKE: return "note_choke";
    case CLAP_EVENT_NOTE_END: return "note_end";
    case CLAP_EVENT_NOTE_EXPRESSION: return "note_expression";
    case CLAP_EVENT_PARAM_VALUE: return "param_value";
    case CLAP_EVENT_PARAM_MOD: return "param_mod";
    case CLAP_EVENT_PARAM_GESTURE_BEGIN: return "param_gesture_begin";
    case CLAP_EVENT_PARAM_GESTURE_END: return "param_gesture_end";
    case CLAP_EVENT_TRANSPORT: return "transport";
    case CLAP_EVENT_MIDI: return "midi";
    case CLAP_EVENT_MIDI_SYSEX: return "midi_sysex";
    case CLAP_EVENT_MIDI2: return "midi2";
    default: return "unknown";
    }
}

// ---------- helpers --------------------------------------------------------

std::string sanitiseFileToken(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                          || c == '.' || c == '-' || c == '_';
        if (keep)
            out.push_back(c);
        else if (c == ' ' || c == '\t')
            out.push_back('_');
    }
    if (out.empty())
        out = "x";
    return out;
}

std::string nowTimestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm {};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string nowIso8601()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm {};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string detectOsName()
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string detectOsVersion()
{
#if defined(_WIN32)
    OSVERSIONINFOEXW info {};
    info.dwOSVersionInfoSize = sizeof(info);
    using FnRtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = reinterpret_cast<FnRtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion"))) {
            if (fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&info)) == 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                              static_cast<unsigned long>(info.dwMajorVersion),
                              static_cast<unsigned long>(info.dwMinorVersion),
                              static_cast<unsigned long>(info.dwBuildNumber));
                return buf;
            }
        }
    }
    return "unknown";
#else
    struct utsname u {};
    if (uname(&u) == 0)
        return std::string(u.release);
    return "unknown";
#endif
}

extern "C" const clap_plugin_entry_t clap_entry;

std::string pluginBinaryDir()
{
#if defined(_WIN32)
    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                              | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&clap_entry), &h)) {
        wchar_t path[MAX_PATH * 2];
        const DWORD len = GetModuleFileNameW(h, path, MAX_PATH * 2);
        if (len > 0) {
            std::wstring w(path, path + len);
            const auto slash = w.find_last_of(L"/\\");
            if (slash != std::wstring::npos)
                w.resize(slash);
            // wide -> utf8
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string out(static_cast<size_t>(n - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
                return out;
            }
        }
    }
    return {};
#else
    Dl_info info {};
    if (dladdr(reinterpret_cast<const void*>(&clap_entry), &info) && info.dli_fname) {
        std::string path = info.dli_fname;
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos)
            path.resize(slash);
        return path;
    }
    return {};
#endif
}

std::string defaultCasesPath()
{
    auto dir = pluginBinaryDir();
    if (dir.empty())
        return {};
    return dir + "/" + kCasesFileName;
}

std::string defaultLogFolder()
{
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return std::string(local) + "\\DAWHostProbe\\logs";

    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wpath)) && wpath) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
        std::string out;
        if (n > 0) {
            out.assign(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out.data(), n, nullptr, nullptr);
        }
        CoTaskMemFree(wpath);
        if (!out.empty())
            return out + "\\DAWHostProbe\\logs";
    }

    if (const char* up = std::getenv("USERPROFILE"); up && *up)
        return std::string(up) + "\\AppData\\Local\\DAWHostProbe\\logs";

    return {};
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/Library/Logs/DAWHostProbe";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg && *xdg)
        return std::string(xdg) + "/daw-host-probe/logs";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/state/daw-host-probe/logs";
    return {};
#endif
}

std::string readFileToString(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------- catalogue ------------------------------------------------------

struct CaseEntry {
    std::string id;
    std::string title;
    std::string purpose;
    std::string expected;
    std::vector<std::string> setup;
    std::vector<std::string> steps;
    std::vector<std::string> references;
};

struct CaseGroup {
    std::string id;
    std::string title;
    std::vector<CaseEntry> cases;
};

std::optional<std::string> readStringMember(const choc::value::ValueView& v, std::string_view name)
{
    if (!v.isObject() || !v.hasObjectMember(name))
        return std::nullopt;
    auto m = v[name];
    if (!m.isString())
        return std::nullopt;
    return std::string(m.getString());
}

std::vector<std::string> readStringArray(const choc::value::ValueView& v, std::string_view name)
{
    std::vector<std::string> out;
    if (!v.isObject() || !v.hasObjectMember(name))
        return out;
    auto arr = v[name];
    if (!arr.isArray())
        return out;
    out.reserve(arr.size());
    for (uint32_t i = 0; i < arr.size(); ++i) {
        auto e = arr[i];
        if (e.isString())
            out.emplace_back(e.getString());
    }
    return out;
}

bool parseCatalogue(std::string_view text, std::vector<CaseGroup>& out, std::string& error)
{
    out.clear();
    error.clear();
    try {
        auto root = choc::json::parse(text);
        if (!root.isObject() || !root.hasObjectMember("groups")) {
            error = "missing 'groups'";
            return false;
        }
        auto groups = root["groups"];
        if (!groups.isArray()) {
            error = "'groups' is not an array";
            return false;
        }
        for (uint32_t gi = 0; gi < groups.size(); ++gi) {
            auto g = groups[gi];
            if (!g.isObject())
                continue;
            CaseGroup cg;
            cg.id = readStringMember(g, "id").value_or("");
            cg.title = readStringMember(g, "title").value_or(cg.id);
            if (cg.id.empty())
                continue;
            if (g.hasObjectMember("cases")) {
                auto cs = g["cases"];
                if (cs.isArray()) {
                    for (uint32_t ci = 0; ci < cs.size(); ++ci) {
                        auto c = cs[ci];
                        if (!c.isObject())
                            continue;
                        CaseEntry e;
                        e.id = readStringMember(c, "id").value_or("");
                        e.title = readStringMember(c, "title").value_or(e.id);
                        if (e.id.empty())
                            continue;
                        e.purpose = readStringMember(c, "purpose").value_or("");
                        e.expected = readStringMember(c, "expected").value_or("");
                        e.setup = readStringArray(c, "setup");
                        e.steps = readStringArray(c, "steps");
                        e.references = readStringArray(c, "references");
                        cg.cases.push_back(std::move(e));
                    }
                }
            }
            out.push_back(std::move(cg));
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

// ---------- session metadata ----------------------------------------------

struct HostMeta {
    std::string daw_name;
    std::string daw_version;
    std::string os_name;
    std::string os_version;
#ifndef DAW_HOST_PROBE_FORMAT
#  define DAW_HOST_PROBE_FORMAT "clap"
#endif
    std::string plugin_format = DAW_HOST_PROBE_FORMAT;
};

// ---------- native pickers (Windows v1) -----------------------------------

#if defined(_WIN32)

std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1)
        return {};
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

std::string browseFolderWin(HWND owner)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool needsUninit = SUCCEEDED(hr);

    std::string result;
    IFileDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IFileDialog, reinterpret_cast<void**>(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = wideToUtf8(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (needsUninit)
        CoUninitialize();
    return result;
}

std::string browseJsonWin(HWND owner)
{
    wchar_t buf[MAX_PATH * 2] {};
    OPENFILENAMEW ofn {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof(buf) / sizeof(wchar_t);
    ofn.lpstrFilter = L"JSON files\0*.json\0All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn))
        return wideToUtf8(buf);
    return {};
}

#endif

std::string browseFolder(void* owner)
{
#if defined(_WIN32)
    return browseFolderWin(static_cast<HWND>(owner));
#else
    (void)owner;
    return {};
#endif
}

std::string browseCasesFile(void* owner)
{
#if defined(_WIN32)
    return browseJsonWin(static_cast<HWND>(owner));
#else
    (void)owner;
    return {};
#endif
}

bool openFolderInFileBrowser(const std::string& folder)
{
    if (folder.empty())
        return false;
#if defined(_WIN32)
    const std::wstring wide = utf8ToWide(folder);
    if (wide.empty())
        return false;
    const auto result = ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
#else
#if defined(__APPLE__)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    pid_t pid {};
    char* const argv[] = { const_cast<char*>(opener), const_cast<char*>(folder.c_str()), nullptr };
    return posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0;
#endif
}

struct MidiNote {
    uint32_t start_ticks;
    uint32_t duration_ticks;
    uint8_t key;
    uint8_t velocity;
};

void appendU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendBytes(std::vector<uint8_t>& out, std::initializer_list<uint8_t> bytes)
{
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void appendVarLen(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t buffer[4] {};
    int index = 3;
    buffer[index] = static_cast<uint8_t>(value & 0x7f);
    while ((value >>= 7) != 0 && index > 0)
        buffer[--index] = static_cast<uint8_t>((value & 0x7f) | 0x80);
    for (; index < 4; ++index)
        out.push_back(buffer[index]);
}

std::vector<uint8_t> makeMidiFile(const std::vector<MidiNote>& notes, uint32_t minimumLengthTicks = 1920)
{
    struct Event {
        uint32_t tick;
        uint8_t status;
        uint8_t key;
        uint8_t velocity;
    };

    std::vector<Event> events;
    events.reserve(notes.size() * 2);
    for (const auto& note : notes) {
        events.push_back({ note.start_ticks, 0x90, note.key, note.velocity });
        events.push_back({ note.start_ticks + note.duration_ticks, 0x80, note.key, 0 });
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        if (a.tick != b.tick)
            return a.tick < b.tick;
        return a.status < b.status;
    });

    std::vector<uint8_t> track;
    appendVarLen(track, 0);
    appendBytes(track, { 0xff, 0x51, 0x03, 0x06, 0x8a, 0x1b }); // 140 BPM.
    appendVarLen(track, 0);
    appendBytes(track, { 0xff, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08 }); // 4/4.

    uint32_t cursor = 0;
    for (const auto& event : events) {
        appendVarLen(track, event.tick - cursor);
        appendBytes(track, { event.status, event.key, event.velocity });
        cursor = event.tick;
    }

    appendVarLen(track, cursor < minimumLengthTicks ? minimumLengthTicks - cursor : 0);
    appendBytes(track, { 0xff, 0x2f, 0x00 });

    std::vector<uint8_t> out;
    appendBytes(out, { 'M', 'T', 'h', 'd' });
    appendU32(out, 6);
    appendU16(out, 0);
    appendU16(out, 1);
    appendU16(out, 480);
    appendBytes(out, { 'M', 'T', 'r', 'k' });
    appendU32(out, static_cast<uint32_t>(track.size()));
    out.insert(out.end(), track.begin(), track.end());
    return out;
}

bool writeBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return f.good();
}

// ---------- HostProbe ------------------------------------------------------

struct HostProbe {
    clap_plugin_t plugin {};
    const clap_host_t* host { nullptr };

    // Session / logging state.
    std::mutex io_mutex;
    FILE* log_file { nullptr };
    std::string current_log_path;
    std::string log_folder;
    std::string cases_path;
    std::string active_case_id;
    HostMeta meta;
    std::vector<CaseGroup> cases;
    std::unordered_set<std::string> captured;
    std::string catalogue_error;
    std::string log_error;

    // Audio / probe state.
    std::array<std::atomic<double>, kParams.size()> param_values {};
    uint64_t block_sequence { 0 };
    uint64_t audio_frame_pos { 0 };
    double sample_rate { 0.0 };
    bool active { false };
    bool processing { false };
    bool have_last_transport { false };
    double last_seconds_pos { 0.0 };
    double last_beats_pos { 0.0 };

    // GUI.
    std::unique_ptr<choc::ui::WebView> webview;
#if defined(_WIN32)
    HWND parent_hwnd { nullptr };
#endif
    bool gui_created { false };
    bool html_loaded { false };
    bool bindings_registered { false };
    uint32_t pending_w { 980 };
    uint32_t pending_h { 640 };

    HostProbe()
    {
        for (size_t i = 0; i < kParams.size(); ++i)
            param_values[i].store(kParams[i].def, std::memory_order_relaxed);

        meta.os_name = detectOsName();
        meta.os_version = detectOsVersion();

        if (const char* path = std::getenv("DAW_HOST_PROBE_LOG")) {
            log_file = std::fopen(path, "a");
            if (log_file)
                current_log_path = path;
        }
        cases_path = defaultCasesPath();
        log_folder = defaultLogFolder();
        loadCatalogueFromDisk();

        log("create", "plugin=daw-host-probe version=%s", kProbeVersion);
    }

    ~HostProbe()
    {
        log("destroy", "plugin=daw-host-probe");
        std::lock_guard lock(io_mutex);
        if (log_file)
            std::fclose(log_file);
        log_file = nullptr;
    }

    // ---- logging primitives -----------------------------------------------

    // Caller must hold io_mutex if the call may race with rotation.
    void writeLogLineLocked(const char* callback, const char* fmt, va_list args)
    {
        if (!log_file)
            return;
        std::fprintf(log_file,
            "probe block=%llu frame=%llu sr=%.1f active=%d processing=%d cb=%s ",
            static_cast<unsigned long long>(block_sequence),
            static_cast<unsigned long long>(audio_frame_pos),
            sample_rate,
            int(active),
            int(processing),
            callback);
        std::vfprintf(log_file, fmt, args);
        std::fprintf(log_file, "\n");
        std::fflush(log_file);
    }

    void log(const char* callback, const char* fmt, ...)
    {
        std::lock_guard lock(io_mutex);
        if (!log_file)
            return;
        va_list args;
        va_start(args, fmt);
        writeLogLineLocked(callback, fmt, args);
        va_end(args);
    }

    static HostProbe* from(const clap_plugin_t* p)
    {
        return static_cast<HostProbe*>(p->plugin_data);
    }

    // ---- log file management ---------------------------------------------

    static std::string buildCaseLogPath(const std::string& folder,
                                        const HostMeta& meta,
                                        const std::string& case_id)
    {
        std::string base = folder;
        if (!base.empty() && base.back() != '/' && base.back() != '\\')
            base += '/';
        std::string name;
        name += sanitiseFileToken(meta.daw_name.empty() ? "host" : meta.daw_name);
        name += '-';
        name += sanitiseFileToken(meta.daw_version.empty() ? "x" : meta.daw_version);
        name += '-';
        name += sanitiseFileToken(meta.plugin_format.empty() ? "clap" : meta.plugin_format);
        name += '-';
        name += sanitiseFileToken(case_id);
        name += '-';
        name += nowTimestamp();
        name += ".log";
        return base + name;
    }

    static std::string buildUniqueCaseLogPath(const std::string& folder,
                                              const HostMeta& meta,
                                              const std::string& case_id)
    {
        const std::string path = buildCaseLogPath(folder, meta, case_id);
        if (!std::filesystem::exists(path))
            return path;

        const std::filesystem::path base(path);
        const auto parent = base.parent_path();
        const auto stem = base.stem().string();
        const auto ext = base.extension().string();
        for (int i = 2; i < 1000; ++i) {
            const auto candidate = parent / (stem + "-" + std::to_string(i) + ext);
            if (!std::filesystem::exists(candidate))
                return candidate.string();
        }
        return path;
    }

    // Caller must hold io_mutex.
    void closeLogFileLocked(const char* reason)
    {
        if (!log_file)
            return;
        if (reason && *reason) {
            std::fprintf(log_file,
                "probe block=%llu frame=%llu sr=%.1f active=%d processing=%d cb=ui_session_close case_id=%s reason=%s wall_time=%s\n",
                static_cast<unsigned long long>(block_sequence),
                static_cast<unsigned long long>(audio_frame_pos),
                sample_rate, int(active), int(processing),
                active_case_id.empty() ? "(none)" : active_case_id.c_str(),
                reason, nowIso8601().c_str());
            std::fflush(log_file);
        }
        std::fclose(log_file);
        log_file = nullptr;
        current_log_path.clear();
    }

    // Caller must hold io_mutex.
    void openLogFileLocked(const std::string& path, const std::string& case_id, const char* mode = "a")
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path(), ec);
        log_file = std::fopen(path.c_str(), mode);
        if (!log_file) {
            log_error = "Failed to open log file: " + path;
            return;
        }
        log_error.clear();
        current_log_path = path;

        const CaseEntry* entry = findCase(case_id);
        const std::string title = entry ? entry->title : std::string {};
        std::fprintf(log_file,
            "probe block=%llu frame=%llu sr=%.1f active=%d processing=%d cb=session_header schema_version=1 case_id=%s case_title=\"%s\" daw=%s daw_version=%s os=%s os_version=%s plugin_format=%s probe_version=%s session_start=%s log_path=%s\n",
            static_cast<unsigned long long>(block_sequence),
            static_cast<unsigned long long>(audio_frame_pos),
            sample_rate, int(active), int(processing),
            case_id.c_str(),
            title.c_str(),
            meta.daw_name.c_str(),
            meta.daw_version.c_str(),
            meta.os_name.c_str(),
            meta.os_version.c_str(),
            meta.plugin_format.c_str(),
            kProbeVersion,
            nowIso8601().c_str(),
            path.c_str());
        std::fflush(log_file);
    }

    void activateCase(const std::string& id)
    {
        std::lock_guard lock(io_mutex);
        if (log_folder.empty()) {
            catalogue_error = "Log folder must be set before activating a case.";
            return;
        }
        if (!findCase(id)) {
            catalogue_error = "Unknown case id: " + id;
            return;
        }
        if (log_file)
            closeLogFileLocked("case_switch");
        active_case_id = id;
        const std::string path = buildUniqueCaseLogPath(log_folder, meta, id);
        openLogFileLocked(path, id);
    }

    void deactivateCase()
    {
        std::lock_guard lock(io_mutex);
        if (log_file)
            closeLogFileLocked("ui_stop_logging");
        active_case_id.clear();
    }

    bool newActiveLog(std::string& pathOut)
    {
        std::lock_guard lock(io_mutex);
        pathOut.clear();
        if (!log_file || active_case_id.empty()) {
            log_error = "Activate a case before starting a new log file.";
            return false;
        }
        const std::string id = active_case_id;
        closeLogFileLocked("ui_new_log_file");
        const std::string path = buildUniqueCaseLogPath(log_folder, meta, id);
        openLogFileLocked(path, id);
        pathOut = current_log_path;
        return !pathOut.empty();
    }

    bool resetActiveLog()
    {
        std::lock_guard lock(io_mutex);
        if (!log_file || current_log_path.empty() || active_case_id.empty()) {
            log_error = "Activate a case before resetting the current log.";
            return false;
        }
        const std::string path = current_log_path;
        const std::string id = active_case_id;
        closeLogFileLocked(nullptr);
        openLogFileLocked(path, id, "w");
        return log_file != nullptr;
    }

    bool openLogFolder()
    {
        if (log_folder.empty()) {
            log_error = "Log folder is empty.";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(log_folder, ec);
        if (!openFolderInFileBrowser(log_folder)) {
            log_error = "Failed to open log folder: " + log_folder;
            return false;
        }
        log_error.clear();
        return true;
    }

    std::filesystem::path midiFolderPath() const
    {
        return std::filesystem::path(log_folder) / "midi";
    }

    bool writeMidiFixtures()
    {
        if (log_folder.empty()) {
            log_error = "Set a log folder before writing MIDI files.";
            return false;
        }

        constexpr uint32_t q = 480;
        constexpr uint32_t e = 240;
        constexpr uint32_t s = 120;

        const auto folder = midiFolderPath();
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec) {
            log_error = "Failed to create MIDI folder: " + folder.string();
            return false;
        }

        const std::vector<std::pair<const char*, std::vector<MidiNote>>> fixtures {
            { "single-c4-anchor.mid", { { 0, q, 60, 100 } } },
            { "sparse-multi-anchor.mid", { { 0, e, 60, 100 }, { q + 2 * s, s, 64, 100 }, { 2 * q + s, e + s, 67, 100 }, { 3 * q + 3 * s, s, 72, 100 } } },
            { "repeated-same-key.mid", { { 0, e, 60, 96 }, { e, e, 60, 96 }, { q, e, 60, 96 }, { q + e, e, 60, 96 } } },
            { "same-sample-transition.mid", { { 0, q, 60, 96 }, { q, q, 64, 96 } } },
            { "overlap-and-choke.mid", { { 0, q + e, 60, 100 }, { e, q, 60, 70 }, { 2 * q, q, 67, 110 } } },
            { "boundary-held.mid", { { 3 * q + e, q, 60, 100 } } },
            { "boundary-adjacent.mid", { { 0, s, 60, 100 }, { s, s, 62, 100 }, { 3 * q + 2 * s, s, 67, 100 }, { 3 * q + 3 * s, s, 72, 100 } } },
            { "velocity-sweep.mid", { { 0, s, 60, 30 }, { e, s, 60, 60 }, { q, s, 60, 90 }, { q + e, s, 60, 120 } } },
            { "pitch-sweep.mid", { { 0, s, 60, 100 }, { e, s, 62, 100 }, { q, s, 64, 100 }, { q + e, s, 67, 100 }, { 2 * q, s, 72, 100 } } },
            { "live-edit-source.mid", { { 0, e, 60, 100 }, { q + 2 * s, s, 64, 100 }, { 2 * q + s, e + s, 67, 100 }, { 3 * q + 3 * s, s, 72, 100 } } },
        };

        for (const auto& fixture : fixtures) {
            const auto path = folder / fixture.first;
            if (!writeBinaryFile(path, makeMidiFile(fixture.second))) {
                log_error = "Failed to write MIDI file: " + path.string();
                return false;
            }
        }

        log_error.clear();
        return true;
    }

    bool openMidiFolder()
    {
        if (!writeMidiFixtures())
            return false;
        if (!openFolderInFileBrowser(midiFolderPath().string())) {
            log_error = "Failed to open MIDI folder: " + midiFolderPath().string();
            return false;
        }
        log_error.clear();
        return true;
    }

    void writeMarker(const char* cb, const char* fmt, ...)
    {
        std::lock_guard lock(io_mutex);
        if (!log_file)
            return;
        va_list args;
        va_start(args, fmt);
        writeLogLineLocked(cb, fmt, args);
        va_end(args);
    }

    void setCaptured(const std::string& id, bool flag)
    {
        if (flag)
            captured.insert(id);
        else
            captured.erase(id);
        writeMarker("ui_capture_marker",
                    "case_id=%s captured=%d wall_time=%s",
                    id.c_str(), int(flag), nowIso8601().c_str());
    }

    void addNote(const std::string& text)
    {
        // Sanitise quotes / newlines so the log line stays parseable.
        std::string cleaned;
        cleaned.reserve(text.size());
        for (char c : text) {
            if (c == '\n' || c == '\r')
                cleaned.push_back(' ');
            else if (c == '"')
                cleaned.push_back('\'');
            else
                cleaned.push_back(c);
        }
        writeMarker("ui_note",
                    "case_id=%s text=\"%s\" wall_time=%s",
                    active_case_id.empty() ? "(none)" : active_case_id.c_str(),
                    cleaned.c_str(),
                    nowIso8601().c_str());
    }

    // ---- catalogue --------------------------------------------------------

    const CaseEntry* findCase(const std::string& id) const
    {
        for (const auto& g : cases)
            for (const auto& c : g.cases)
                if (c.id == id)
                    return &c;
        return nullptr;
    }

    void loadCatalogueFromDisk()
    {
        cases.clear();
        catalogue_error.clear();
        if (cases_path.empty()) {
            catalogue_error = "No catalogue path set.";
            return;
        }
        const std::string text = readFileToString(cases_path);
        if (text.empty()) {
            catalogue_error = "Catalogue file is missing or empty: " + cases_path;
            return;
        }
        std::string err;
        if (!parseCatalogue(text, cases, err))
            catalogue_error = "Failed to parse catalogue: " + err;
    }

    // ---- metadata auto-fill ----------------------------------------------

    void autodetectMeta()
    {
        if (host) {
            if (host->name && *host->name)
                meta.daw_name = host->name;
            if (host->version && *host->version)
                meta.daw_version = host->version;
        }
        meta.os_name = detectOsName();
        meta.os_version = detectOsVersion();
    }

    // ---- state persistence ------------------------------------------------

    std::string serialiseState() const
    {
        auto root = choc::value::createObject("probe_state");
        root.addMember("version", static_cast<int32_t>(1));
        root.addMember("log_folder", log_folder);
        root.addMember("cases_path", cases_path);
        root.addMember("active_case_id", active_case_id);

        auto m = choc::value::createObject("meta");
        m.addMember("daw_name", meta.daw_name);
        m.addMember("daw_version", meta.daw_version);
        m.addMember("os_name", meta.os_name);
        m.addMember("os_version", meta.os_version);
        m.addMember("plugin_format", meta.plugin_format);
        root.addMember("meta", std::move(m));

        auto cap = choc::value::createEmptyArray();
        for (const auto& id : captured)
            cap.addArrayElement(id);
        root.addMember("captured", std::move(cap));

        return choc::json::toString(root, false);
    }

    bool loadStateFromString(std::string_view text)
    {
        try {
            auto root = choc::json::parse(text);
            if (!root.isObject())
                return false;
            if (auto v = readStringMember(root, "log_folder"); v && !v->empty())
                log_folder = *v;
            if (auto v = readStringMember(root, "cases_path"))
                cases_path = *v;
            std::string desired_case;
            if (auto v = readStringMember(root, "active_case_id"))
                desired_case = *v;

            if (root.hasObjectMember("meta") && root["meta"].isObject()) {
                auto m = root["meta"];
                if (auto v = readStringMember(m, "daw_name"))      meta.daw_name = *v;
                if (auto v = readStringMember(m, "daw_version"))   meta.daw_version = *v;
                if (auto v = readStringMember(m, "os_name"))       meta.os_name = *v;
                if (auto v = readStringMember(m, "os_version"))    meta.os_version = *v;
                // plugin_format is determined at compile time per wrapper target;
                // ignore any persisted value so a project saved under one wrapper
                // (e.g. CLAP) shows the right format when reopened under another.
            }

            captured.clear();
            for (const auto& id : readStringArray(root, "captured"))
                captured.insert(id);

            loadCatalogueFromDisk();

            // Re-open a fresh log file rather than appending to a stale one.
            if (!desired_case.empty() && !log_folder.empty() && findCase(desired_case))
                activateCase(desired_case);

            pushStatusToUi();
            return true;
        } catch (...) {
            return false;
        }
    }

    // ---- UI status snapshot -----------------------------------------------

    std::string buildStatusJson()
    {
        auto root = choc::value::createObject("probe_status");
        root.addMember("log_folder", log_folder);
        root.addMember("cases_path", cases_path);
        root.addMember("active_case_id", active_case_id);
        root.addMember("active_log_path", current_log_path);
        root.addMember("catalogue_error", catalogue_error);
        root.addMember("log_error", log_error);
        root.addMember("probe_version", std::string(kProbeVersion));

        auto m = choc::value::createObject("meta");
        m.addMember("daw_name", meta.daw_name);
        m.addMember("daw_version", meta.daw_version);
        m.addMember("os_name", meta.os_name);
        m.addMember("os_version", meta.os_version);
        m.addMember("plugin_format", meta.plugin_format);
        root.addMember("meta", std::move(m));

        auto groups = choc::value::createEmptyArray();
        for (const auto& g : cases) {
            auto go = choc::value::createObject("group");
            go.addMember("id", g.id);
            go.addMember("title", g.title);
            auto cs = choc::value::createEmptyArray();
            for (const auto& c : g.cases) {
                auto co = choc::value::createObject("case");
                co.addMember("id", c.id);
                co.addMember("title", c.title);
                co.addMember("purpose", c.purpose);
                co.addMember("expected", c.expected);
                auto setupArr = choc::value::createEmptyArray();
                for (const auto& s : c.setup) setupArr.addArrayElement(s);
                co.addMember("setup", std::move(setupArr));
                auto stepsArr = choc::value::createEmptyArray();
                for (const auto& s : c.steps) stepsArr.addArrayElement(s);
                co.addMember("steps", std::move(stepsArr));
                auto refsArr = choc::value::createEmptyArray();
                for (const auto& s : c.references) refsArr.addArrayElement(s);
                co.addMember("references", std::move(refsArr));
                cs.addArrayElement(co);
            }
            go.addMember("cases", std::move(cs));
            groups.addArrayElement(go);
        }
        root.addMember("groups", std::move(groups));

        auto cap = choc::value::createEmptyArray();
        for (const auto& id : captured) cap.addArrayElement(id);
        root.addMember("captured", std::move(cap));

        return choc::json::toString(root, false);
    }

    void pushStatusToUi();

    // ---- existing audio-thread hooks --------------------------------------

    void logTransport(const char* callback, const char* source, const clap_event_transport_t* t, uint32_t event_index = UINT32_MAX)
    {
        if (!t) {
            log(callback, "transport_snapshot source=%s present=0", source);
            return;
        }

        const bool has_tempo = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
        const bool has_beats = (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
        const bool has_seconds = (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0;
        const bool has_tsig = (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) != 0;
        const bool playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        const bool recording = (t->flags & CLAP_TRANSPORT_IS_RECORDING) != 0;
        const bool loop_active = (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) != 0;
        const bool pre_roll = (t->flags & CLAP_TRANSPORT_IS_WITHIN_PRE_ROLL) != 0;
        const bool valid_seconds_loop = has_seconds && t->loop_end_seconds > t->loop_start_seconds;
        const bool valid_beats_loop = has_beats && t->loop_end_beats > t->loop_start_beats;

        log(callback,
            "transport_snapshot source=%s event_index=%u flags=0x%x playing=%d recording=%d loop_active=%d pre_roll=%d has_tempo=%d has_seconds=%d has_beats=%d has_time_signature=%d tempo=%.9f tempo_inc=%.12f song_seconds=%.9f song_beats=%.9f loop_start_seconds=%.9f loop_end_seconds=%.9f loop_start_beats=%.9f loop_end_beats=%.9f valid_song_seconds=%d valid_song_beats=%d valid_loop_seconds=%d valid_loop_beats=%d bar_start_beats=%.9f bar_number=%d tsig=%u/%u",
            source,
            event_index,
            unsigned(t->flags),
            int(playing),
            int(recording),
            int(loop_active),
            int(pre_roll),
            int(has_tempo),
            int(has_seconds),
            int(has_beats),
            int(has_tsig),
            t->tempo,
            t->tempo_inc,
            secTime(t->song_pos_seconds),
            beatTime(t->song_pos_beats),
            secTime(t->loop_start_seconds),
            secTime(t->loop_end_seconds),
            beatTime(t->loop_start_beats),
            beatTime(t->loop_end_beats),
            int(has_seconds),
            int(has_beats),
            int(valid_seconds_loop),
            int(valid_beats_loop),
            beatTime(t->bar_start),
            int(t->bar_number),
            unsigned(t->tsig_num),
            unsigned(t->tsig_denom));
    }

    void logPredictedWrap(const clap_event_transport_t* t, uint32_t frames)
    {
        if (!t || frames == 0 || sample_rate <= 1.0)
            return;
        if ((t->flags & CLAP_TRANSPORT_IS_PLAYING) == 0 || (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE) == 0)
            return;

        const bool has_seconds_loop = (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
            && t->loop_end_seconds > t->loop_start_seconds;
        const bool has_beats_loop = (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            && (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
            && t->tempo > 1.0
            && t->loop_end_beats > t->loop_start_beats;

        double pos = 0.0;
        double loop_start = 0.0;
        double loop_end = 0.0;
        double frames_to_end = 0.0;
        const char* source = "none";
        if (has_seconds_loop) {
            source = "seconds";
            pos = secTime(t->song_pos_seconds);
            loop_start = secTime(t->loop_start_seconds);
            loop_end = secTime(t->loop_end_seconds);
            frames_to_end = (loop_end - pos) * sample_rate;
        } else if (has_beats_loop) {
            source = "beats";
            pos = beatTime(t->song_pos_beats);
            loop_start = beatTime(t->loop_start_beats);
            loop_end = beatTime(t->loop_end_beats);
            frames_to_end = (loop_end - pos) * 60.0 / t->tempo * sample_rate;
        } else {
            return;
        }

        if (loop_end <= pos || frames_to_end <= 0.0 || frames_to_end > static_cast<double>(frames))
            return;

        const uint32_t wrap_frame = std::clamp<uint32_t>(static_cast<uint32_t>(frames_to_end + 0.999999), 1u, frames);
        log("process",
            "predicted_loop_wrap block=%llu wrap_frame=%u previous_song_seconds=%.9f previous_song_beats=%.9f current_position=%.9f loop_start=%.9f loop_end=%.9f source=%s frames_to_end=%.6f",
            static_cast<unsigned long long>(block_sequence),
            wrap_frame,
            have_last_transport ? last_seconds_pos : 0.0,
            have_last_transport ? last_beats_pos : 0.0,
            pos,
            loop_start,
            loop_end,
            source,
            frames_to_end);
    }

    void rememberTransport(const clap_event_transport_t* t)
    {
        if (!t)
            return;
        last_seconds_pos = secTime(t->song_pos_seconds);
        last_beats_pos = beatTime(t->song_pos_beats);
        have_last_transport = true;
    }

    void logEventPayload(const char* callback, const clap_event_header_t* event, const char* source, uint32_t index)
    {
        if (!event)
            return;
        log(callback,
            "raw_event source=%s index=%u block=%llu time=%u type=%u type_name=%s space=%u flags=0x%x size=%u",
            source,
            index,
            static_cast<unsigned long long>(block_sequence),
            event->time,
            unsigned(event->type),
            eventTypeName(event->type),
            unsigned(event->space_id),
            unsigned(event->flags),
            unsigned(event->size));

        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return;

        switch (event->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE:
        case CLAP_EVENT_NOTE_END: {
            const auto* ev = reinterpret_cast<const clap_event_note_t*>(event);
            log(callback,
                "note_event source=%s index=%u time=%u type_name=%s note_id=%d port=%d channel=%d key=%d velocity=%.9f",
                source, index, event->time, eventTypeName(event->type), ev->note_id,
                int(ev->port_index), int(ev->channel), int(ev->key), ev->velocity);
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION: {
            const auto* ev = reinterpret_cast<const clap_event_note_expression_t*>(event);
            log(callback,
                "note_expression source=%s index=%u time=%u expression_id=%d note_id=%d port=%d channel=%d key=%d value=%.9f",
                source, index, event->time, int(ev->expression_id), ev->note_id,
                int(ev->port_index), int(ev->channel), int(ev->key), ev->value);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE: {
            const auto* ev = reinterpret_cast<const clap_event_param_value_t*>(event);
            log(callback,
                "param_value source=%s index=%u time=%u param_id=%u note_id=%d port=%d channel=%d key=%d value=%.9f",
                source, index, event->time, unsigned(ev->param_id), ev->note_id,
                int(ev->port_index), int(ev->channel), int(ev->key), ev->value);
            const uint32_t idx = paramIndex(ev->param_id);
            if (idx != UINT32_MAX)
                param_values[idx].store(std::clamp(ev->value, kParams[idx].min, kParams[idx].max), std::memory_order_release);
            break;
        }
        case CLAP_EVENT_PARAM_MOD: {
            const auto* ev = reinterpret_cast<const clap_event_param_mod_t*>(event);
            log(callback,
                "param_mod source=%s index=%u time=%u param_id=%u note_id=%d port=%d channel=%d key=%d amount=%.9f",
                source, index, event->time, unsigned(ev->param_id), ev->note_id,
                int(ev->port_index), int(ev->channel), int(ev->key), ev->amount);
            break;
        }
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END: {
            const auto* ev = reinterpret_cast<const clap_event_param_gesture_t*>(event);
            log(callback,
                "param_gesture source=%s index=%u time=%u type_name=%s param_id=%u",
                source, index, event->time, eventTypeName(event->type), unsigned(ev->param_id));
            break;
        }
        case CLAP_EVENT_TRANSPORT: {
            logTransport(callback, source, reinterpret_cast<const clap_event_transport_t*>(event), index);
            rememberTransport(reinterpret_cast<const clap_event_transport_t*>(event));
            break;
        }
        case CLAP_EVENT_MIDI: {
            const auto* ev = reinterpret_cast<const clap_event_midi_t*>(event);
            log(callback,
                "midi source=%s index=%u time=%u port=%u status=0x%02x data1=%u data2=%u bytes=%02x %02x %02x",
                source, index, event->time, unsigned(ev->port_index), unsigned(ev->data[0]),
                unsigned(ev->data[1]), unsigned(ev->data[2]), unsigned(ev->data[0]),
                unsigned(ev->data[1]), unsigned(ev->data[2]));
            break;
        }
        case CLAP_EVENT_MIDI_SYSEX: {
            const auto* ev = reinterpret_cast<const clap_event_midi_sysex_t*>(event);
            const uint32_t n = std::min<uint32_t>(ev->size, 16);
            char bytes[16 * 3 + 1] {};
            for (uint32_t i = 0; i < n && ev->buffer; ++i)
                std::snprintf(bytes + i * 3, sizeof(bytes) - i * 3, "%02x%s", unsigned(ev->buffer[i]), i + 1 == n ? "" : " ");
            log(callback,
                "midi_sysex source=%s index=%u time=%u port=%u size=%u first_bytes=%s",
                source, index, event->time, unsigned(ev->port_index), unsigned(ev->size), bytes);
            break;
        }
        case CLAP_EVENT_MIDI2: {
            const auto* ev = reinterpret_cast<const clap_event_midi2_t*>(event);
            log(callback,
                "midi2 source=%s index=%u time=%u port=%u words=%08x %08x %08x %08x",
                source, index, event->time, unsigned(ev->port_index), unsigned(ev->data[0]),
                unsigned(ev->data[1]), unsigned(ev->data[2]), unsigned(ev->data[3]));
            break;
        }
        default:
            break;
        }
    }

    void copyAudio(const clap_process_t* process)
    {
        for (uint32_t port = 0; port < process->audio_outputs_count; ++port) {
            clap_audio_buffer_t& out = process->audio_outputs[port];
            const clap_audio_buffer_t* in = port < process->audio_inputs_count ? &process->audio_inputs[port] : nullptr;
            for (uint32_t ch = 0; ch < out.channel_count; ++ch) {
                float* out32 = out.data32 ? out.data32[ch] : nullptr;
                double* out64 = out.data64 ? out.data64[ch] : nullptr;
                const float* in32 = (in && in->data32 && ch < in->channel_count) ? in->data32[ch] : nullptr;
                const double* in64 = (in && in->data64 && ch < in->channel_count) ? in->data64[ch] : nullptr;
                for (uint32_t i = 0; i < process->frames_count; ++i) {
                    const uint32_t in_index = (in && (in->constant_mask & (uint64_t(1) << ch))) ? 0 : i;
                    if (out32)
                        out32[i] = in32 ? in32[in_index] : (in64 ? static_cast<float>(in64[in_index]) : 0.0f);
                    if (out64)
                        out64[i] = in64 ? in64[in_index] : (in32 ? static_cast<double>(in32[in_index]) : 0.0);
                }
            }
            out.constant_mask = 0;
        }
    }
};

// ---------- embedded UI HTML ----------------------------------------------

const char* kProbeHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8" />
<title>DAW Host Probe</title>
<style>
  :root {
    --bg: #15171b;
    --panel: #1e2127;
    --panel2: #262a31;
    --border: #353a44;
    --text: #d6d8dc;
    --muted: #8b929c;
    --accent: #5aa9ff;
    --warn: #ffb86b;
    --ok: #6bd482;
  }
  html, body { margin: 0; padding: 0; height: 100%; background: var(--bg); color: var(--text); font: 13px/1.4 -apple-system, "Segoe UI", Roboto, sans-serif; }
  body { display: flex; flex-direction: column; }
  h2, h3, h4 { margin: 0 0 6px; font-weight: 600; }
  h2 { font-size: 14px; color: var(--accent); }
  h3 { font-size: 13px; color: var(--accent); }
  h4 { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; margin-top: 12px; }
  section { padding: 10px 14px; border-bottom: 1px solid var(--border); }
  section.last { border-bottom: none; }
  label { color: var(--muted); font-size: 12px; }
  input[type=text] { background: var(--panel2); color: var(--text); border: 1px solid var(--border); border-radius: 3px; padding: 4px 6px; font: inherit; }
  button { background: var(--panel2); color: var(--text); border: 1px solid var(--border); border-radius: 3px; padding: 4px 10px; font: inherit; cursor: pointer; }
  button:hover { background: #2f343d; }
  button:disabled { opacity: 0.45; cursor: default; }
  button:disabled:hover { background: var(--panel2); }
  button.primary { background: var(--accent); border-color: var(--accent); color: #fff; }
  button.primary:hover { background: #4895ee; }
  .row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 6px; }
  .row > label { min-width: 60px; }
  .grow { flex: 1; min-width: 120px; }
  .meta-grid { display: grid; grid-template-columns: max-content 1fr max-content 1fr; gap: 6px 10px; align-items: center; }
  .body { display: flex; flex: 1; min-height: 0; }
  .pane-cat { width: 320px; border-right: 1px solid var(--border); overflow: auto; padding: 10px 12px; }
  .pane-detail { flex: 1; overflow: auto; padding: 12px 16px; }
  .group { margin-bottom: 10px; }
  .group-title { font-weight: 600; color: var(--accent); margin-bottom: 4px; }
  .setup-profile-title { margin: 8px 0 3px; color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.04em; }
  .case-row { display: grid; grid-template-columns: 18px 18px 36px 1fr; align-items: center; gap: 4px; padding: 2px 0; cursor: pointer; }
  .case-row:hover { background: var(--panel2); border-radius: 3px; }
  .case-row.active { background: #2a3a52; border-radius: 3px; }
  .case-id { color: var(--muted); font-family: ui-monospace, "Cascadia Mono", Menlo, monospace; font-size: 11px; }
  .case-title { font-size: 12px; }
  .pill { display: inline-block; padding: 1px 6px; border-radius: 8px; font-size: 11px; }
  .pill.active { background: var(--accent); color: #fff; }
  .pill.captured { background: var(--ok); color: #102; }
  .chip { display: inline-block; padding: 2px 7px; margin-right: 4px; background: var(--panel2); border: 1px solid var(--border); border-radius: 10px; font-size: 11px; cursor: pointer; }
  .chip:hover { background: #303642; }
  .checklist { margin: 0; padding: 0; list-style: none; }
  .checklist li { display: flex; gap: 6px; align-items: flex-start; padding: 2px 0; }
  .checklist input { margin-top: 3px; }
  .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 4px; padding: 10px 12px; margin-bottom: 10px; }
  .status-line { font-family: ui-monospace, monospace; font-size: 11px; color: var(--muted); }
  .error { color: var(--warn); }
  .footer { font-size: 11px; color: var(--muted); padding: 6px 14px; border-top: 1px solid var(--border); }
  .radio { width: 14px; height: 14px; border-radius: 50%; border: 1px solid var(--muted); display: inline-block; }
  .radio.on { background: var(--accent); border-color: var(--accent); }
</style>
</head>
<body>
  <section>
    <h2>DAW Host Probe</h2>
    <div class="status-line" id="probe-version"></div>
    <div class="status-line error" id="catalogue-error" style="display:none"></div>
    <div class="status-line error" id="log-error" style="display:none"></div>
  </section>

  <section>
    <h3>Session</h3>
    <div class="meta-grid">
      <label>DAW</label><input id="m-daw" type="text" />
      <label>Version</label><input id="m-dawver" type="text" />
      <label>OS</label><input id="m-os" type="text" />
      <label>Version</label><input id="m-osver" type="text" />
      <label>Format</label>
      <div>
        <label><input type="radio" name="fmt" value="clap" /> CLAP</label>
        <label><input type="radio" name="fmt" value="vst3" /> VST3</label>
        <label><input type="radio" name="fmt" value="auv2" /> AUv2</label>
        <label><input type="radio" name="fmt" value="standalone" /> Standalone</label>
      </div>
      <div></div>
      <div><button id="auto-meta">Auto-detect from host</button></div>
    </div>
  </section>

  <section>
    <h3>Capture</h3>
    <div class="row">
      <label>Folder</label>
      <input id="log-folder" type="text" class="grow" />
      <button id="browse-folder">Browse</button>
      <button id="open-log-folder">Open folder</button>
    </div>
    <div class="row">
      <label>Status</label>
      <span id="logging-status" class="status-line grow">Not logging</span>
      <button id="new-log">Start another take</button>
      <button id="reset-log">Clear this take</button>
    </div>
    <div class="row">
      <label>File</label>
      <span id="active-log-path" class="status-line grow">(no active log)</span>
    </div>
    <div class="row">
      <label>MIDI files</label>
      <span class="status-line grow">Write fixtures, then drag them from the folder into the DAW.</span>
      <button id="write-midi-files">Write MIDI files</button>
      <button id="open-midi-folder">Open MIDI folder</button>
    </div>
  </section>

  <div class="body">
    <div class="pane-cat">
      <div class="row" style="margin-bottom:8px">
        <h3 style="margin:0;flex:1">Cases</h3>
        <button id="reload-cases">Reload</button>
        <button id="browse-cases">Browse JSON</button>
      </div>
      <div class="row" style="margin-bottom:8px">
        <input id="cases-path" type="text" class="grow" />
      </div>
      <div id="catalogue"></div>
    </div>

    <div class="pane-detail" id="detail">
      <div class="status-line">Select a case from the catalogue to view its instructions.</div>
    </div>
  </div>

  <div class="footer status-line" id="footer"></div>

<script>
)HTML"
R"HTML(
  const $ = (id) => document.getElementById(id);
  let state = null;
  let selectedCaseId = null;

  async function refresh() {
    const json = await window.ui_get_status();
    try { state = JSON.parse(json); } catch (_) { state = null; }
    if (!state) return;
    render();
  }

  function metaRadio(value) {
    document.querySelectorAll('input[name=fmt]').forEach(r => { r.checked = (r.value === value); });
  }

  function render() {
    $("probe-version").textContent = "probe v" + state.probe_version;
    if (state.catalogue_error) {
      $("catalogue-error").style.display = "block";
      $("catalogue-error").textContent = state.catalogue_error;
    } else {
      $("catalogue-error").style.display = "none";
    }
    if (state.log_error) {
      $("log-error").style.display = "block";
      $("log-error").textContent = state.log_error;
    } else {
      $("log-error").style.display = "none";
    }

    if (document.activeElement?.id !== "m-daw")    $("m-daw").value    = state.meta.daw_name || "";
    if (document.activeElement?.id !== "m-dawver") $("m-dawver").value = state.meta.daw_version || "";
    if (document.activeElement?.id !== "m-os")     $("m-os").value     = state.meta.os_name || "";
    if (document.activeElement?.id !== "m-osver")  $("m-osver").value  = state.meta.os_version || "";
    metaRadio(state.meta.plugin_format || "clap");

    if (document.activeElement?.id !== "log-folder") $("log-folder").value = state.log_folder || "";
    if (document.activeElement?.id !== "cases-path") $("cases-path").value = state.cases_path || "";
    $("active-log-path").textContent = state.active_log_path || "(no active log)";
    const isLogging = !!(state.active_case_id && state.active_log_path);
    const activeCase = findCase(state.active_case_id);
    const selectedCase = findCase(selectedCaseId);
    $("logging-status").textContent = isLogging
      ? ("Capturing " + state.active_case_id + (activeCase ? " - " + activeCase.title : ""))
      : (selectedCase ? ("Ready: " + selectedCase.id + " - " + selectedCase.title) : "Select a case, then start a capture");
    $("new-log").disabled = !isLogging;
    $("reset-log").disabled = !isLogging;
    $("open-log-folder").disabled = !(state.log_folder || "").trim();
    $("write-midi-files").disabled = !(state.log_folder || "").trim();
    $("open-midi-folder").disabled = !(state.log_folder || "").trim();
    $("footer").textContent = "active case: " + (state.active_case_id || "(none)") + " · done: " + (state.captured?.length || 0);

    renderCatalogue();
    renderDetail();
  }

  function renderCatalogue() {
    const root = $("catalogue");
    root.innerHTML = "";
    const captured = new Set(state.captured || []);
    for (const g of state.groups || []) {
      const groupDiv = document.createElement("div");
      groupDiv.className = "group";
      const title = document.createElement("div");
      title.className = "group-title";
      title.textContent = g.title;
      groupDiv.appendChild(title);
      const profileOrder = [];
      const byProfile = new Map();
      for (const c of g.cases || []) {
        const profile = caseSetupProfile(c);
        if (!byProfile.has(profile)) {
          byProfile.set(profile, []);
          profileOrder.push(profile);
        }
        byProfile.get(profile).push(c);
      }
      for (const profile of profileOrder) {
        if (g.cases.length > 1) {
          const profileTitle = document.createElement("div");
          profileTitle.className = "setup-profile-title";
          profileTitle.textContent = profile;
          groupDiv.appendChild(profileTitle);
        }
      for (const c of byProfile.get(profile)) {
        const row = document.createElement("div");
        row.className = "case-row" + (state.active_case_id === c.id ? " active" : "");
        row.dataset.caseId = c.id;

        const radio = document.createElement("span");
        radio.className = "radio" + (state.active_case_id === c.id ? " on" : "");
        radio.title = state.active_case_id === c.id
          ? "Finish capture"
          : (state.active_case_id ? "Switch capture to this case" : "Start capture");
        radio.addEventListener("click", (e) => {
          e.stopPropagation();
          if (state.active_case_id === c.id)
            window.ui_deactivate_case().then(refresh);
          else
            window.ui_activate_case(c.id).then(refresh);
        });
        row.appendChild(radio);

        const cap = document.createElement("input");
        cap.type = "checkbox";
        cap.checked = captured.has(c.id);
        cap.title = "Done";
        cap.addEventListener("click", (e) => e.stopPropagation());
        cap.addEventListener("change", () => {
          window.ui_set_captured(c.id, cap.checked).then(refresh);
        });
        row.appendChild(cap);

        const id = document.createElement("span");
        id.className = "case-id";
        id.textContent = c.id;
        row.appendChild(id);

        const ttl = document.createElement("span");
        ttl.className = "case-title";
        ttl.textContent = c.title;
        row.appendChild(ttl);

        row.addEventListener("click", () => {
          selectedCaseId = c.id;
          render();
        });
        groupDiv.appendChild(row);
      }
      }
      root.appendChild(groupDiv);
    }
  }

  function findCase(id) {
    if (!id || !state.groups) return null;
    for (const g of state.groups)
      for (const c of g.cases)
        if (c.id === id) return c;
    return null;
  }

  function setupSectionTitle(item) {
    const label = String(item || "").split(":")[0].trim().toLowerCase();
    if (["audio", "tempo", "arrangement loop", "loop", "loop start", "loop end", "loop length", "initial playhead", "initial transport state"].includes(label))
      return "Initial setup";
    if (label.startsWith("midi"))
      return "MIDI setup";
    if (label.startsWith("plugin") || label.includes("buffer") || label.includes("sample") || label.includes("automation"))
      return "Plugin/setup details";
    return "Case-specific setup";
  }

  function setupValue(c, label) {
    const prefix = label.toLowerCase() + ":";
    for (const item of c.setup || []) {
      const text = String(item || "");
      if (text.toLowerCase().startsWith(prefix))
        return text.slice(prefix.length).trim();
    }
    return "";
  }

  function caseSetupProfile(c) {
    if ((c.id || "").startsWith("M"))
      return "MIDI pattern reference";
    const loop = setupValue(c, "Loop");
    const loopStart = setupValue(c, "Loop start") || loop;
    const loopEnd = setupValue(c, "Loop end") || loop;
    const arrangement = setupValue(c, "Arrangement loop");
    if (loopStart.includes("3.1.1") && loopEnd.includes("4.1.1"))
      return "Amen one-bar loop, later position";
    if (loopStart.includes("choose") || loopEnd.includes("choose") || (setupValue(c, "Loop length") || "").includes("buffer"))
      return "Amen short/custom loop";
    if (loopStart.includes("clip") || loopEnd.includes("clip"))
      return "Clip/session loop setup";
    if (loopStart.includes("pattern") || arrangement.includes("tracker"))
      return "Tracker/pattern loop setup";
    if (loopStart.includes("2.1.1") && loopEnd.includes("3.1.1"))
      return "Amen one-bar loop, bars 2-3";
    return "Custom or host-specific setup";
  }

  function groupSetup(items) {
    const order = ["Initial setup", "MIDI setup", "Plugin/setup details", "Case-specific setup"];
    const grouped = new Map();
    for (const title of order) grouped.set(title, []);
    for (const item of items || []) {
      grouped.get(setupSectionTitle(item)).push(item);
    }
    return order
      .map((title) => ({ title, items: grouped.get(title) }))
      .filter((section) => section.items.length);
  }

  function renderDetail() {
    const detail = $("detail");
    const selectedId = selectedCaseId || state.active_case_id;
    const c = findCase(selectedId);
    if (!c) {
      detail.innerHTML = '<div class="status-line">Select a case from the catalogue to view its instructions.</div>';
      return;
    }

    const isActive = state.active_case_id === c.id;
    const hasActiveCapture = !!state.active_case_id;
    const captured = (state.captured || []).includes(c.id);
    const captureButtonText = isActive
      ? "Finish capture"
      : (hasActiveCapture ? "Switch capture to this case" : "Start capture");

    let html = "";
    html += `<h2>${escapeHtml(c.id)} — ${escapeHtml(c.title)}</h2>`;
    html += `<div class="row">`;
    if (isActive) html += `<span class="pill active">CAPTURING</span>`;
    html += `<label><input id="cap-toggle" type="checkbox" ${captured ? "checked" : ""} /> done</label>`;
    html += `<button id="capture-btn" class="primary">${captureButtonText}</button>`;
    if (isActive) html += `<button id="finish-done-btn">Finish and mark done</button>`;
    html += `</div>`;

    if (c.purpose) {
      html += `<h4>Purpose</h4><div class="panel">${escapeHtml(c.purpose)}</div>`;
    }
    if (c.setup?.length) {
      for (const section of groupSetup(c.setup)) {
        html += `<h4>${escapeHtml(section.title)}</h4><ul class="checklist">`;
        for (const s of section.items) html += `<li><input type="checkbox" /><span>${escapeHtml(s)}</span></li>`;
        html += `</ul>`;
      }
    }
    if (c.steps?.length) {
      html += `<h4>Operator steps</h4><ol class="checklist">`;
      for (const s of c.steps) html += `<li><input type="checkbox" /><span>${escapeHtml(s)}</span></li>`;
      html += `</ol>`;
    }
    if (c.expected) {
      html += `<h4>Expected</h4><div class="panel">${escapeHtml(c.expected)}</div>`;
    }
    if (c.references?.length) {
      html += `<h4>Related</h4><div>`;
      for (const r of c.references) html += `<span class="chip" data-ref="${escapeHtml(r)}">${escapeHtml(r)}</span>`;
      html += `</div>`;
    }
    html += `<h4>Note</h4><div class="row"><input id="case-note" type="text" class="grow" placeholder="${isActive ? "Annotate this capture" : "Start capture to log notes"}" ${isActive ? "" : "disabled"} /><button id="note-btn" ${isActive ? "" : "disabled"}>Log note</button></div>`;

    detail.innerHTML = html;

    detail.querySelector("#cap-toggle").addEventListener("change", (e) => {
      window.ui_set_captured(c.id, e.target.checked).then(refresh);
    });
    detail.querySelector("#capture-btn").addEventListener("click", () => {
      if (isActive)
        window.ui_deactivate_case().then(refresh);
      else
        window.ui_activate_case(c.id).then(refresh);
    });
    const finishDone = detail.querySelector("#finish-done-btn");
    if (finishDone) {
      finishDone.addEventListener("click", () => {
        window.ui_set_captured(c.id, true)
          .then(() => window.ui_deactivate_case())
          .then(refresh);
      });
    }
    detail.querySelectorAll(".chip").forEach((chip) => {
      chip.addEventListener("click", () => {
        selectedCaseId = chip.dataset.ref;
        render();
      });
    });
    if (isActive) {
      const note = detail.querySelector("#case-note");
      const btn = detail.querySelector("#note-btn");
      const send = () => {
        const t = note.value.trim();
        if (!t) return;
        window.ui_add_note(t).then(() => { note.value = ""; refresh(); });
      };
      btn.addEventListener("click", send);
      note.addEventListener("keydown", (e) => { if (e.key === "Enter") send(); });
    }
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]));
  }

  function bindMeta(id, key) {
    $(id).addEventListener("change", () => {
      window.ui_set_meta(key, $(id).value).then(refresh);
    });
  }

  function init() {
    bindMeta("m-daw",    "daw_name");
    bindMeta("m-dawver", "daw_version");
    bindMeta("m-os",     "os_name");
    bindMeta("m-osver",  "os_version");
    document.querySelectorAll('input[name=fmt]').forEach(r => {
      r.addEventListener("change", () => {
        if (r.checked) window.ui_set_meta("plugin_format", r.value).then(refresh);
      });
    });

    $("auto-meta").addEventListener("click", () => window.ui_autodetect_meta().then(refresh));
    $("browse-folder").addEventListener("click", async () => {
      const p = await window.ui_browse_log_folder();
      if (p) refresh();
    });
    $("open-log-folder").addEventListener("click", () => window.ui_open_log_folder().then(refresh));
    $("log-folder").addEventListener("change", () => {
      window.ui_set_log_folder($("log-folder").value).then(refresh);
    });
    $("new-log").addEventListener("click", () => window.ui_new_active_log().then(refresh));
    $("reset-log").addEventListener("click", () => window.ui_reset_active_log().then(refresh));
    $("write-midi-files").addEventListener("click", () => window.ui_write_midi_files().then(refresh));
    $("open-midi-folder").addEventListener("click", () => window.ui_open_midi_folder().then(refresh));
    $("reload-cases").addEventListener("click", () => window.ui_reload_cases().then(refresh));
    $("browse-cases").addEventListener("click", async () => {
      const p = await window.ui_browse_cases_file();
      if (p) refresh();
    });
    $("cases-path").addEventListener("change", () => {
      window.ui_set_cases_path($("cases-path").value).then(refresh);
    });

    window.ui_refresh = refresh;
    refresh();
  }

  if (document.readyState === "loading")
    document.addEventListener("DOMContentLoaded", init);
  else
    init();
</script>
</body>
</html>
)HTML";

// ---------- UI binding glue -----------------------------------------------

choc::value::Value valueFromString(const std::string& s)
{
    return choc::value::Value(s);
}

void HostProbe::pushStatusToUi()
{
    if (!webview)
        return;
    // Schedule on JS side via the polling refresh; here we just call the function if defined.
    webview->evaluateJavascript("if (window.ui_refresh) window.ui_refresh();");
}

void bindUi(HostProbe& self)
{
    auto& wv = *self.webview;

    wv.bind("ui_get_status", [&self](const choc::value::ValueView&) {
        return valueFromString(self.buildStatusJson());
    });

    wv.bind("ui_set_log_folder", [&self](const choc::value::ValueView& args) {
        if (args.isArray() && args.size() > 0 && args[0].isString())
            self.log_folder = std::string(args[0].getString());
        return choc::value::Value();
    });

    wv.bind("ui_browse_log_folder", [&self](const choc::value::ValueView&) {
#if defined(_WIN32)
        void* owner = self.parent_hwnd;
#else
        void* owner = nullptr;
#endif
        std::string p = browseFolder(owner);
        if (!p.empty())
            self.log_folder = p;
        return p.empty() ? choc::value::Value() : valueFromString(p);
    });

    wv.bind("ui_open_log_folder", [&self](const choc::value::ValueView&) {
        self.openLogFolder();
        return choc::value::Value();
    });

    wv.bind("ui_write_midi_files", [&self](const choc::value::ValueView&) {
        self.writeMidiFixtures();
        return choc::value::Value();
    });

    wv.bind("ui_open_midi_folder", [&self](const choc::value::ValueView&) {
        self.openMidiFolder();
        return choc::value::Value();
    });

    wv.bind("ui_new_active_log", [&self](const choc::value::ValueView&) {
        std::string path;
        self.newActiveLog(path);
        return path.empty() ? choc::value::Value() : valueFromString(path);
    });

    wv.bind("ui_reset_active_log", [&self](const choc::value::ValueView&) {
        self.resetActiveLog();
        return choc::value::Value();
    });

    wv.bind("ui_stop_logging", [&self](const choc::value::ValueView&) {
        self.deactivateCase();
        return choc::value::Value();
    });

    wv.bind("ui_set_cases_path", [&self](const choc::value::ValueView& args) {
        if (args.isArray() && args.size() > 0 && args[0].isString()) {
            self.cases_path = std::string(args[0].getString());
            self.loadCatalogueFromDisk();
        }
        return choc::value::Value();
    });

    wv.bind("ui_browse_cases_file", [&self](const choc::value::ValueView&) {
#if defined(_WIN32)
        void* owner = self.parent_hwnd;
#else
        void* owner = nullptr;
#endif
        std::string p = browseCasesFile(owner);
        if (!p.empty()) {
            self.cases_path = p;
            self.loadCatalogueFromDisk();
        }
        return p.empty() ? choc::value::Value() : valueFromString(p);
    });

    wv.bind("ui_reload_cases", [&self](const choc::value::ValueView&) {
        self.loadCatalogueFromDisk();
        return choc::value::Value();
    });

    wv.bind("ui_set_meta", [&self](const choc::value::ValueView& args) {
        if (args.isArray() && args.size() >= 2 && args[0].isString() && args[1].isString()) {
            const std::string key(args[0].getString());
            const std::string val(args[1].getString());
            if (key == "daw_name")           self.meta.daw_name = val;
            else if (key == "daw_version")   self.meta.daw_version = val;
            else if (key == "os_name")       self.meta.os_name = val;
            else if (key == "os_version")    self.meta.os_version = val;
            else if (key == "plugin_format") self.meta.plugin_format = val;
        }
        return choc::value::Value();
    });

    wv.bind("ui_autodetect_meta", [&self](const choc::value::ValueView&) {
        self.autodetectMeta();
        return choc::value::Value();
    });

    wv.bind("ui_activate_case", [&self](const choc::value::ValueView& args) {
        if (args.isArray() && args.size() > 0 && args[0].isString())
            self.activateCase(std::string(args[0].getString()));
        return choc::value::Value();
    });

    wv.bind("ui_deactivate_case", [&self](const choc::value::ValueView&) {
        self.deactivateCase();
        return choc::value::Value();
    });

    wv.bind("ui_set_captured", [&self](const choc::value::ValueView& args) {
        if (args.isArray() && args.size() >= 2 && args[0].isString()) {
            const std::string id(args[0].getString());
            const bool flag = args[1].isBool() ? args[1].getBool() : false;
            self.setCaptured(id, flag);
        }
        return choc::value::Value();
    });

    wv.bind("ui_add_note", [&self](const choc::value::ValueView& args) {
        if (args.isArray() && args.size() > 0 && args[0].isString())
            self.addNote(std::string(args[0].getString()));
        return choc::value::Value();
    });
}

// ---------- CLAP extensions -----------------------------------------------

const clap_plugin_audio_ports_t kAudioPortsExt = {
    .count = [](const clap_plugin_t*, bool) -> uint32_t { return 1; },
    .get = [](const clap_plugin_t*, uint32_t index, bool is_input, clap_audio_port_info_t* info) -> bool {
        if (index != 0)
            return false;
        std::memset(info, 0, sizeof(*info));
        info->id = is_input ? 0 : 1024;
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "Input" : "Output");
        return true;
    },
};

const clap_plugin_note_ports_t kNotePortsExt = {
    .count = [](const clap_plugin_t*, bool is_input) -> uint32_t { return is_input ? 1u : 0u; },
    .get = [](const clap_plugin_t*, uint32_t index, bool is_input, clap_note_port_info_t* info) -> bool {
        if (!is_input || index != 0)
            return false;
        std::memset(info, 0, sizeof(*info));
        info->id = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE | CLAP_NOTE_DIALECT_MIDI2;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf(info->name, sizeof(info->name), "%s", "Probe Note Input");
        return true;
    },
};

const clap_plugin_params_t kParamsExt = {
    .count = [](const clap_plugin_t*) -> uint32_t { return static_cast<uint32_t>(kParams.size()); },
    .get_info = [](const clap_plugin_t*, uint32_t index, clap_param_info_t* info) -> bool {
        if (index >= kParams.size())
            return false;
        std::memset(info, 0, sizeof(*info));
        const auto& p = kParams[index];
        info->id = p.id;
        info->flags = p.flags;
        info->min_value = p.min;
        info->max_value = p.max;
        info->default_value = p.def;
        std::snprintf(info->name, sizeof(info->name), "%s", p.name);
        std::snprintf(info->module, sizeof(info->module), "%s", "Probe");
        return true;
    },
    .get_value = [](const clap_plugin_t* plugin, clap_id id, double* value) -> bool {
        const uint32_t idx = paramIndex(id);
        if (idx == UINT32_MAX)
            return false;
        *value = HostProbe::from(plugin)->param_values[idx].load(std::memory_order_acquire);
        return true;
    },
    .value_to_text = [](const clap_plugin_t*, clap_id id, double value, char* display, uint32_t size) -> bool {
        if (paramIndex(id) == UINT32_MAX)
            return false;
        std::snprintf(display, size, "%.6f", value);
        return true;
    },
    .text_to_value = [](const clap_plugin_t*, clap_id id, const char* display, double* value) -> bool {
        const uint32_t idx = paramIndex(id);
        if (idx == UINT32_MAX)
            return false;
        *value = std::clamp(std::atof(display), kParams[idx].min, kParams[idx].max);
        return true;
    },
    .flush = [](const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*) {
        auto* self = HostProbe::from(plugin);
        const uint32_t n = in ? in->size(in) : 0;
        self->log("params_flush", "flush_events count=%u source=params_flush", n);
        for (uint32_t i = 0; i < n; ++i)
            self->logEventPayload("params_flush", in->get(in, i), "params_flush", i);
    },
};

const ClapPluginAsVst3 kVst3Ext = {
    .getNumMIDIChannels = [](const clap_plugin*, uint32_t) -> uint32_t { return 16; },
    .supportedNoteExpressions = [](const clap_plugin*) -> uint32_t { return (1u << 7) - 1u; },
};

const clap_plugin_state_t kStateExt = {
    .save = [](const clap_plugin_t* p, const clap_ostream_t* stream) -> bool {
        auto* self = HostProbe::from(p);
        const std::string blob = self->serialiseState();
        size_t written = 0;
        const char* data = blob.data();
        const size_t total = blob.size();
        while (written < total) {
            const auto n = stream->write(stream, data + written, total - written);
            if (n <= 0)
                return false;
            written += static_cast<size_t>(n);
        }
        return true;
    },
    .load = [](const clap_plugin_t* p, const clap_istream_t* stream) -> bool {
        auto* self = HostProbe::from(p);
        std::string blob;
        char buf[4096];
        for (;;) {
            const auto n = stream->read(stream, buf, sizeof(buf));
            if (n < 0)
                return false;
            if (n == 0)
                break;
            blob.append(buf, static_cast<size_t>(n));
        }
        return self->loadStateFromString(blob);
    },
};

// ---------- GUI ext --------------------------------------------------------

constexpr const char* kGuiApiPreferred =
#if defined(_WIN32)
    CLAP_WINDOW_API_WIN32
#elif defined(__APPLE__)
    CLAP_WINDOW_API_COCOA
#else
    CLAP_WINDOW_API_X11
#endif
    ;

const clap_plugin_gui_t kGuiExt = {
    .is_api_supported = [](const clap_plugin_t*, const char* api, bool is_floating) -> bool {
        if (is_floating)
            return false;
        return std::strcmp(api, kGuiApiPreferred) == 0;
    },
    .get_preferred_api = [](const clap_plugin_t*, const char** api, bool* is_floating) -> bool {
        *api = kGuiApiPreferred;
        *is_floating = false;
        return true;
    },
    .create = [](const clap_plugin_t* p, const char* api, bool is_floating) -> bool {
        if (is_floating || std::strcmp(api, kGuiApiPreferred) != 0)
            return false;
        auto* self = HostProbe::from(p);
        if (self->gui_created)
            return true;
        try {
            choc::ui::WebView::Options opts;
            opts.enableDebugMode = false;
            // WebView2 is created asynchronously: coreWebView is null until
            // webviewIsReady fires. choc::WebView::bind() requires a live
            // coreWebView (it calls AddScriptToExecuteOnDocumentCreated and
            // silently drops the binding if it fails), so bindUi MUST run
            // inside webviewIsReady — not after make_unique. setHTML must
            // also wait until after bindUi, so the page's first call to
            // window.ui_get_status() finds a registered binding.
            opts.webviewIsReady = [self](choc::ui::WebView& wv) {
                if (!self->bindings_registered) {
                    bindUi(*self);
                    self->bindings_registered = true;
                }
                if (!self->html_loaded) {
                    wv.setHTML(kProbeHtml);
                    self->html_loaded = true;
                }
#if defined(_WIN32)
                if (HWND child = static_cast<HWND>(wv.getViewHandle()))
                    SetWindowPos(child, nullptr, 0, 0,
                                 static_cast<int>(self->pending_w),
                                 static_cast<int>(self->pending_h),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
#endif
            };
            self->webview = std::make_unique<choc::ui::WebView>(opts);
            if (!self->webview->loadedOK()) {
                self->webview.reset();
                return false;
            }
            // Defensive fallback: if a platform fires webviewIsReady
            // synchronously inside the constructor and we somehow missed it,
            // or if isReady() is true by the time we get here, drive bind +
            // load now. In practice on Win/Mac/Linux the callback is async.
            if (self->webview->isReady()) {
                if (!self->bindings_registered) {
                    bindUi(*self);
                    self->bindings_registered = true;
                }
                if (!self->html_loaded) {
                    self->webview->setHTML(kProbeHtml);
                    self->html_loaded = true;
                }
            }
            self->gui_created = true;
            self->log("gui_create", "gui_create");
            return true;
        } catch (...) {
            self->webview.reset();
            return false;
        }
    },
    .destroy = [](const clap_plugin_t* p) {
        auto* self = HostProbe::from(p);
        self->log("gui_destroy", "gui_destroy");
        self->webview.reset();
        self->gui_created = false;
        self->html_loaded = false;
        self->bindings_registered = false;
#if defined(_WIN32)
        self->parent_hwnd = nullptr;
#endif
    },
    .set_scale = [](const clap_plugin_t*, double) -> bool { return true; },
    .get_size = [](const clap_plugin_t*, uint32_t* width, uint32_t* height) -> bool {
        *width = 980;
        *height = 640;
        return true;
    },
    .can_resize = [](const clap_plugin_t*) -> bool { return true; },
    .get_resize_hints = [](const clap_plugin_t*, clap_gui_resize_hints_t* hints) -> bool {
        hints->can_resize_horizontally = true;
        hints->can_resize_vertically = true;
        hints->preserve_aspect_ratio = false;
        hints->aspect_ratio_width = 1;
        hints->aspect_ratio_height = 1;
        return true;
    },
    .adjust_size = [](const clap_plugin_t*, uint32_t*, uint32_t*) -> bool { return true; },
    .set_size = [](const clap_plugin_t* p, uint32_t width, uint32_t height) -> bool {
        auto* self = HostProbe::from(p);
        self->pending_w = width;
        self->pending_h = height;
        if (!self->webview)
            return false;
#if defined(_WIN32)
        if (HWND child = static_cast<HWND>(self->webview->getViewHandle()))
            SetWindowPos(child, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
#else
        (void)width; (void)height;
        return true;
#endif
    },
    .set_parent = [](const clap_plugin_t* p, const clap_window_t* window) -> bool {
        auto* self = HostProbe::from(p);
        if (!self->webview || !window)
            return false;
#if defined(_WIN32)
        HWND parent = reinterpret_cast<HWND>(window->win32);
        HWND child = static_cast<HWND>(self->webview->getViewHandle());
        if (!parent || !child)
            return false;
        self->parent_hwnd = parent;
        SetParent(child, parent);
        LONG_PTR style = GetWindowLongPtr(child, GWL_STYLE);
        style = (style & ~(WS_POPUP | WS_OVERLAPPEDWINDOW)) | WS_CHILD | WS_VISIBLE;
        SetWindowLongPtr(child, GWL_STYLE, style);
        SetWindowPos(child, nullptr, 0, 0,
                     static_cast<int>(self->pending_w),
                     static_cast<int>(self->pending_h),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ShowWindow(child, SW_SHOW);
        return true;
#else
        (void)self; (void)window;
        return true;
#endif
    },
    .set_transient = [](const clap_plugin_t*, const clap_window_t*) -> bool { return false; },
    .suggest_title = [](const clap_plugin_t*, const char*) {},
    .show = [](const clap_plugin_t* p) -> bool {
        auto* self = HostProbe::from(p);
        if (!self->webview)
            return false;
#if defined(_WIN32)
        if (HWND child = static_cast<HWND>(self->webview->getViewHandle()))
            ShowWindow(child, SW_SHOW);
#endif
        return true;
    },
    .hide = [](const clap_plugin_t* p) -> bool {
        auto* self = HostProbe::from(p);
        if (!self->webview)
            return false;
#if defined(_WIN32)
        if (HWND child = static_cast<HWND>(self->webview->getViewHandle()))
            ShowWindow(child, SW_HIDE);
#endif
        return true;
    },
};

// ---------- plugin descriptor ---------------------------------------------

const clap_plugin_descriptor_t kDescriptor = {
    .clap_version = CLAP_VERSION_INIT,
    .id = "io.github.daw-host-probe",
    .name = "DAW Host Probe",
    .vendor = "DAW Host Probe",
    .url = "",
    .manual_url = "",
    .support_url = "",
    .version = "0.1.0",
    .description = "Diagnostic CLAP/VST3 plugin that logs DAW host behaviour.",
    .features = kFeatures,
};

const clap_plugin_t kPluginVTable = {
    .desc = &kDescriptor,
    .plugin_data = nullptr,
    .init = [](const clap_plugin_t* p) -> bool {
        auto* self = HostProbe::from(p);
        // Auto-detect host metadata when host pointer is finally usable.
        if (self->host) {
            if (self->meta.daw_name.empty() && self->host->name && *self->host->name)
                self->meta.daw_name = self->host->name;
            if (self->meta.daw_version.empty() && self->host->version && *self->host->version)
                self->meta.daw_version = self->host->version;
        }
        self->log("init", "init");
        return true;
    },
    .destroy = [](const clap_plugin_t* p) { delete HostProbe::from(p); },
    .activate = [](const clap_plugin_t* p, double sr, uint32_t min_frames, uint32_t max_frames) -> bool {
        auto* self = HostProbe::from(p);
        self->sample_rate = sr;
        self->active = true;
        self->audio_frame_pos = 0;
        self->block_sequence = 0;
        self->have_last_transport = false;
        self->log("activate", "activate sample_rate=%.9f min_frames=%u max_frames=%u", sr, min_frames, max_frames);
        return true;
    },
    .deactivate = [](const clap_plugin_t* p) {
        auto* self = HostProbe::from(p);
        self->log("deactivate", "deactivate");
        self->active = false;
    },
    .start_processing = [](const clap_plugin_t* p) -> bool {
        auto* self = HostProbe::from(p);
        self->processing = true;
        self->log("start_processing", "start_processing");
        return true;
    },
    .stop_processing = [](const clap_plugin_t* p) {
        auto* self = HostProbe::from(p);
        self->log("stop_processing", "stop_processing");
        self->processing = false;
    },
    .reset = [](const clap_plugin_t* p) {
        auto* self = HostProbe::from(p);
        self->log("reset", "reset");
        self->audio_frame_pos = 0;
        self->block_sequence = 0;
        self->have_last_transport = false;
    },
    .process = [](const clap_plugin_t* p, const clap_process_t* process) -> clap_process_status {
        auto* self = HostProbe::from(p);
        ++self->block_sequence;
        const uint32_t event_count = process->in_events ? process->in_events->size(process->in_events) : 0;
        self->log("process",
            "process_block phase=start block=%llu steady_time=%lld start_frame=%llu frames=%u input_events=%u audio_inputs=%u audio_outputs=%u transport_present=%d mode_hint=unknown",
            static_cast<unsigned long long>(self->block_sequence),
            static_cast<long long>(process->steady_time),
            static_cast<unsigned long long>(self->audio_frame_pos),
            process->frames_count,
            event_count,
            process->audio_inputs_count,
            process->audio_outputs_count,
            int(process->transport != nullptr));
        self->logTransport("process", "block_start", process->transport);
        self->logPredictedWrap(process->transport, process->frames_count);

        for (uint32_t i = 0; i < event_count; ++i)
            self->logEventPayload("process", process->in_events->get(process->in_events, i), "process_event", i);

        self->copyAudio(process);
        self->rememberTransport(process->transport);
        self->audio_frame_pos += process->frames_count;
        self->log("process",
            "process_block phase=end block=%llu end_frame=%llu frames=%u",
            static_cast<unsigned long long>(self->block_sequence),
            static_cast<unsigned long long>(self->audio_frame_pos),
            process->frames_count);
        return CLAP_PROCESS_CONTINUE;
    },
    .get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
        if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
            return &kAudioPortsExt;
        if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
            return &kNotePortsExt;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
            return &kParamsExt;
        if (std::strcmp(id, CLAP_EXT_STATE) == 0)
            return &kStateExt;
        if (std::strcmp(id, CLAP_EXT_GUI) == 0)
            return &kGuiExt;
        if (std::strcmp(id, kClapPluginAsVst3Ext) == 0)
            return &kVst3Ext;
        return nullptr;
    },
    .on_main_thread = [](const clap_plugin_t*) {},
};

const clap_plugin_factory_t kFactory = {
    .get_plugin_count = [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    .get_plugin_descriptor = [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* {
        return index == 0 ? &kDescriptor : nullptr;
    },
    .create_plugin = [](const clap_plugin_factory_t*, const clap_host_t* host, const char* plugin_id) -> const clap_plugin_t* {
        if (!clap_version_is_compatible(host->clap_version))
            return nullptr;
        if (std::strcmp(plugin_id, kDescriptor.id) != 0)
            return nullptr;
        auto* self = new HostProbe();
        self->host = host;
        if (host) {
            if (host->name && *host->name)
                self->meta.daw_name = host->name;
            if (host->version && *host->version)
                self->meta.daw_version = host->version;
        }
        self->plugin = kPluginVTable;
        self->plugin.plugin_data = self;
        return &self->plugin;
    },
};

}

extern "C" const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = [](const char*) -> bool { return true; },
    .deinit = []() {},
    .get_factory = [](const char* factory_id) -> const void* {
        return std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
    },
};
