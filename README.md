# daw-host-probe

A small, free, open-source diagnostic plugin that records exactly what a DAW
sends to a plugin under common and awkward workflows.

It does **no audio processing**. It only logs:

- Process-block boundaries, frame counters, and live/offline hints.
- Full transport snapshots and transport events.
- Raw input events before the plugin handles them — notes, MIDI bytes,
  parameter and automation events, expressions.
- Predicted and observed loop wraps.
- Lifecycle calls (activate, deactivate, start/stop processing, reset, destroy).

The goal is broad, comparable evidence about how each host behaves — especially
around MIDI sidechain delivery, transport flags, loop wraps, note-id reuse, and
offline render — none of which the CLAP or VST3 specs pin down precisely.

The probe is **CLAP-first** and wrapped to **VST3** (and AUv2 on macOS) via
[`free-audio/clap-wrapper`](https://github.com/free-audio/clap-wrapper).

## Status

Alpha. The case library at `daw-host-probe-cases.json` and `doc/daw-log-capture-cases.md` is v1. Capture
contributions are welcome from any host on any OS — see [Contributing
captures](#contributing-captures) below.

## License

GPL-3.0-or-later. The VST3 build is distributed under GPLv3 per Steinberg's VST3
SDK dual license.

---

## Install

### Pre-built binaries

Grab the latest release from the [Releases page](../../releases) (Windows,
macOS, Linux). Each release contains:

- `daw-host-probe.clap`
- `daw-host-probe.vst3`
- `daw-host-probe-cases.json` (case library; sits next to the binary)
- `samples/140 BPM Loop 5 - The Winstons - Amen.wav`

Copy the plugin to the standard location for your OS:

| OS      | CLAP                                  | VST3                                          |
|---------|---------------------------------------|-----------------------------------------------|
| Windows | `%COMMONPROGRAMFILES%\CLAP\`          | `%COMMONPROGRAMFILES%\VST3\`                  |
| macOS   | `/Library/Audio/Plug-Ins/CLAP/` or `~/Library/Audio/Plug-Ins/CLAP/` | `/Library/Audio/Plug-Ins/VST3/` or `~/Library/Audio/Plug-Ins/VST3/` |
| Linux   | `~/.clap/`                            | `~/.vst3/`                                    |

The `daw-host-probe-cases.json` file must sit next to the plugin binary so the
probe can find it. The release zip is structured for that already.

### Build from source

```bash
git clone https://github.com/<owner>/daw-host-probe.git
cd daw-host-probe
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Optional: pass `-DCLAP_DIR=<path>` and `-DVST3_DIR=<path>` to copy the built
plugin straight into your DAW's plugin folder. CLAP-only:
`-DWITH_VST3=OFF`.

Toolchain: CMake ≥ 3.20, a C++23 compiler (MSVC 19.40+, Clang 16+, GCC 13+).
All other dependencies (clap, clap-wrapper, choc) are fetched automatically.

## Where logs are written

The probe writes a separate log file per capture take to:

| OS      | Path                                                   |
|---------|--------------------------------------------------------|
| Windows | `%LOCALAPPDATA%\DAWHostProbe\logs\`                    |
| macOS   | `~/Library/Logs/DAWHostProbe/`                         |
| Linux   | `$XDG_STATE_HOME/daw-host-probe/logs/` (default `~/.local/state/daw-host-probe/logs/`) |

You can also open the log folder from the plugin's UI.

---

## Capturing a session

1. **Set up a project.** Place
   `samples/140 BPM Loop 5 - The Winstons - Amen.wav` on an audio track at bar
   1.1.1, set tempo to 140 BPM, set the arrangement loop from bar 2.1.1 to bar
   3.1.1. Add the probe to that track. Route a MIDI track as a sidechain into
   the probe.
2. **Pick a case.** The probe's UI lists every case from
   `daw-host-probe-cases.json`. Start with **Level 1** cases (required for
   every host).
3. **Follow the steps for that case.** Each case specifies a setup, a sequence
   of operator actions, and a wait-after-stop. Most cases take 30–90 seconds.
4. **Stop capture.** The probe writes one log file per take.
5. **Repeat for every Level-1 case** in your DAW + plugin format
   (CLAP/VST3/AUv2). Move on to Level 2/3 if you have time.

The full prose protocol is in [`doc/daw-log-capture-cases.md`](doc/daw-log-capture-cases.md).

### What to record alongside the logs

When you submit, please include:

- DAW name and exact version (e.g. `REAPER 7.34`).
- Plugin format used (`CLAP`, `VST3`, `AUv2`).
- OS and OS version.
- Sample rate and buffer size.
- Whether the take was live playback or offline render.
- Any deviations you made from the case steps.

A template `manifest.json` ships in the release zip — fill it in, drop it next
to your logs.

---

## Contributing captures

The collected logs and findings are published openly so every plugin developer
working on MIDI sidechain or transport-sensitive DSP can use them. Two ways to
contribute:

### Option A — GitHub issue (lowest friction)

1. Open an issue on this repo with the title `Capture: <host> <version> <format> — <os>`.
2. Attach a single zip containing the log files for one host/format/OS combo.
3. Paste the manifest JSON inline, or include it inside the zip.
4. Apply the `capture` label.

### Option B — Pull request to the captures repo

For larger or recurring contributors, the raw captures live in a sister repo:
[`daw-host-probe-captures`](https://github.com/<owner>/daw-host-probe-captures).
Fork it, drop your logs in the directory layout below, and open a PR.

```
captures/
  <host-slug>/                      e.g. reaper, ableton-live, bitwig, renoise
    <host-version>/                 e.g. 7.34, 12.1.5
      <format>/                     clap | vst3 | auv2
        <os>/                       windows | macos | linux
          <case-id>/                e.g. L1.1
            <YYYYMMDD-HHMM>-<contributor>/
              manifest.json
              <log files>
```

### Option C — Email

If you can't use GitHub, email the zip to `<contact email>`. Same content as
option A.

### Privacy

Logs are plain text and contain only host-side event data — no audio, no
project content, no personal information. The probe does not phone home.
Before submitting, feel free to open the log files and inspect or redact
anything you don't want public. By submitting, you license the logs under
**CC0** so they can be redistributed and analysed freely.

---

## Published data

The collected captures and the analysis derived from them are published in two
places:

- **Raw captures** — [`daw-host-probe-captures`](https://github.com/<owner>/daw-host-probe-captures),
  organised by host / version / format / OS / case as above. Each capture is
  immutable once merged; corrections go in as a new dated folder.
- **Findings** — [`docs/findings/`](docs/findings/) on this repo. One Markdown
  page per host, plus a top-level cross-host comparison table. Findings are
  rebuilt from the raw captures and cite the capture filenames they're drawn
  from.

A GitHub Pages site rendering the findings will be linked here once the first
host's Level-1 sweep is complete.

## Credits

Contributors of captures are listed in `CAPTURERS.md` (opt-in — say so in your
issue or PR if you'd like to be credited).

## Contact

- Issues / captures: GitHub issue tracker.
- Other: `<contact email>`.
