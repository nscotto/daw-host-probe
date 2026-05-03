cmake_minimum_required(VERSION 3.20)
project(daw_host_probe VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# clap-wrapper forces its targets to /MT; match it globally so the wrapped
# probe doesn't hit MT/MD link-time conflicts on MSVC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Build everything position-independent. clap-wrapper's static libraries
# (base-sdk-vst3, clap-wrapper-shared-detail, …) get linked into our shared
# modules; without -fPIC the Linux linker errors out with "relocation R_X86_64_PC32
# … can not be used when making a shared object".
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Enable Objective-C / Objective-C++ at the top level on Apple platforms so
# CMAKE_OBJCXX_COMPILE_OBJECT is populated before clap-wrapper's AUv2 wrapper
# (which needs OBJCXX) is configured. Without this, universal-binary
# generation fails with "CMAKE_OBJCXX_COMPILE_OBJECT not set".
if (APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

# choc's WebView on Linux pulls in GTK3 + WebKit2GTK.
if (UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  pkg_check_modules(WEBKIT2 REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
endif()

# ---------- third-party deps ----------

include(cmake/clap_wrapper.cmake)    # provides `clap` interface target + clap-wrapper helpers
include(cmake/choc.cmake)            # header-only utility library

# ---------- probe target ----------

set(PROBE_PLUGIN_NAME daw_host_probe)
set(PROBE_OUTPUT_NAME daw-host-probe)
set(PROBE_CLAP_ENTRY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/host_probe_plugin.cc")
set(PROBE_CASES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/daw-host-probe-cases.json")

# Base implementation library. Used as the IMPL_TARGET for clap-wrapper —
# the wrapped CLAP target produces the canonical daw-host-probe.clap, so
# this library is just a plain shared lib (no .clap extension) to avoid
# fighting over the same output filename.
add_library(${PROBE_PLUGIN_NAME} SHARED
  ${PROBE_CLAP_ENTRY_SOURCE}
)
target_include_directories(${PROBE_PLUGIN_NAME} PRIVATE
  ${choc_SOURCE_DIR}
)
if (TARGET clap)
  target_link_libraries(${PROBE_PLUGIN_NAME} PRIVATE clap)
endif()
if (WIN32)
  target_link_libraries(${PROBE_PLUGIN_NAME} PRIVATE Comdlg32 Ole32 Shell32 User32)
elseif (APPLE)
  target_link_libraries(${PROBE_PLUGIN_NAME} PRIVATE
    "-framework Foundation"
    "-framework AppKit"
    "-framework WebKit"
    objc
  )
elseif (UNIX AND NOT APPLE)
  target_link_libraries(${PROBE_PLUGIN_NAME} PRIVATE
    dl
    PkgConfig::GTK3
    PkgConfig::WEBKIT2
  )
endif()

# ---------- wrapped formats via free-audio/clap-wrapper ----------

option(WITH_VST3 "Build VST3 wrapper" ON)
option(WITH_STANDALONE "Build standalone wrapper" OFF)

if (COMMAND make_clapfirst_plugins)
  # clap-wrapper requires CLAP in PLUGIN_FORMATS — it errors with
  # "You must build a clap when making clap-first" otherwise. The wrapped
  # CLAP target is what ships to users.
  set(WRAPPED_FORMATS CLAP)
  if (WITH_VST3)
    list(APPEND WRAPPED_FORMATS VST3)
  endif()
  if (WITH_STANDALONE)
    list(APPEND WRAPPED_FORMATS STANDALONE)
  endif()
  if (APPLE)
    list(APPEND WRAPPED_FORMATS AUV2)
  endif()

  make_clapfirst_plugins(
    TARGET_NAME ${PROBE_PLUGIN_NAME}_wrapped
    IMPL_TARGET ${PROBE_PLUGIN_NAME}
    OUTPUT_NAME "${PROBE_OUTPUT_NAME}"
    ENTRY_SOURCE "${PROBE_CLAP_ENTRY_SOURCE}"
    BUNDLE_IDENTIFIER "io.github.daw-host-probe"
    BUNDLE_VERSION "0.1.0"
    PLUGIN_FORMATS ${WRAPPED_FORMATS}
    WINDOWS_FOLDER_VST3 TRUE
    AUV2_MANUFACTURER_NAME "DAW Host Probe"
    AUV2_MANUFACTURER_CODE "DHPB"
    AUV2_SUBTYPE_CODE "Hpb1"
    AUV2_INSTRUMENT_TYPE "aufx"
    MACOS_EMBEDDED_CLAP_LOCATION "${CMAKE_BINARY_DIR}/${PROBE_OUTPUT_NAME}.clap"
    MACOS_DISABLE_CODE_SIGNING TRUE
  )

  # clap-wrapper recompiles ENTRY_SOURCE inside each wrapped target without
  # inheriting our include dirs — push them through manually.
  foreach(fmt vst3 auv2 standalone clap)
    set(probe_wrapped_target ${PROBE_PLUGIN_NAME}_wrapped_${fmt})
    if (TARGET ${probe_wrapped_target})
      target_include_directories(${probe_wrapped_target} PRIVATE
        ${choc_SOURCE_DIR}
      )
      target_compile_definitions(${probe_wrapped_target} PRIVATE
        DAW_HOST_PROBE_FORMAT="${fmt}"
      )
      if (TARGET clap)
        target_link_libraries(${probe_wrapped_target} PRIVATE clap)
      endif()
      if (WIN32)
        target_link_libraries(${probe_wrapped_target} PRIVATE Comdlg32 Ole32 Shell32 User32)
      elseif (APPLE)
        target_link_libraries(${probe_wrapped_target} PRIVATE
          "-framework Foundation"
          "-framework AppKit"
          "-framework WebKit"
          objc
        )
      elseif (UNIX AND NOT APPLE)
        target_link_libraries(${probe_wrapped_target} PRIVATE
          dl
          PkgConfig::GTK3
          PkgConfig::WEBKIT2
        )
      endif()
      if (fmt STREQUAL "vst3")
        set_target_properties(${probe_wrapped_target} PROPERTIES PREFIX "")
      endif()
    endif()
  endforeach()

  if (CLAP_DIR AND TARGET ${PROBE_PLUGIN_NAME}_wrapped_clap)
    if (APPLE)
      # macOS CLAP is a bundle directory.
      set(PROBE_CLAP_DEST ${CLAP_DIR}/${PROBE_OUTPUT_NAME}.clap)
      add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_clap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          $<TARGET_BUNDLE_DIR:${PROBE_PLUGIN_NAME}_wrapped_clap>
          ${PROBE_CLAP_DEST}
        COMMENT "Installing ${PROBE_OUTPUT_NAME}.clap bundle to ${CLAP_DIR}"
        VERBATIM
      )
      add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_clap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROBE_CASES_FILE}
          ${PROBE_CLAP_DEST}/Contents/MacOS/daw-host-probe-cases.json
        COMMENT "Installing daw-host-probe-cases.json into ${PROBE_OUTPUT_NAME}.clap bundle"
        VERBATIM
      )
    else()
      # Linux/Windows CLAP is a flat shared lib with .clap extension.
      set(PROBE_CLAP_DEST ${CLAP_DIR}/${PROBE_OUTPUT_NAME}.clap)
      add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_clap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:${PROBE_PLUGIN_NAME}_wrapped_clap>
          ${PROBE_CLAP_DEST}
        COMMENT "Installing ${PROBE_OUTPUT_NAME}.clap to ${CLAP_DIR}"
        VERBATIM
      )
      add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_clap POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${PROBE_CASES_FILE}
          ${CLAP_DIR}/daw-host-probe-cases.json
        COMMENT "Installing daw-host-probe-cases.json next to ${PROBE_OUTPUT_NAME}.clap"
        VERBATIM
      )
    endif()
  endif()

  if (VST3_DIR AND TARGET ${PROBE_PLUGIN_NAME}_wrapped_vst3)
    if (WIN32)
      set(PROBE_VST3_BUNDLE_DEST ${VST3_DIR}/${PROBE_OUTPUT_NAME}.vst3)
      set(PROBE_VST3_BIN_SUBDIR x86_64-win)
      add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_vst3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          $<TARGET_FILE_DIR:${PROBE_PLUGIN_NAME}_wrapped_vst3>/../..
          ${PROBE_VST3_BUNDLE_DEST}
      )
    else()
      set(PROBE_VST3_BUNDLE_DEST ${VST3_DIR}/${PROBE_OUTPUT_NAME}.vst3)
      if (APPLE)
        set(PROBE_VST3_BIN_SUBDIR MacOS)
      else()
        set(PROBE_VST3_BIN_SUBDIR x86_64-linux)
      endif()
      add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_vst3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
          $<TARGET_BUNDLE_DIR:${PROBE_PLUGIN_NAME}_wrapped_vst3>
          ${PROBE_VST3_BUNDLE_DEST}
      )
    endif()
    # Cases JSON must sit next to the binary so pluginBinaryDir() finds it.
    add_custom_command(TARGET ${PROBE_PLUGIN_NAME}_wrapped_vst3 POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROBE_CASES_FILE}
        ${PROBE_VST3_BUNDLE_DEST}/Contents/${PROBE_VST3_BIN_SUBDIR}/daw-host-probe-cases.json
      COMMENT "Installing daw-host-probe-cases.json into ${PROBE_OUTPUT_NAME}.vst3 bundle"
      VERBATIM
    )
  endif()
endif()

message(STATUS "${PROBE_PLUGIN_NAME} configured (CXX${CMAKE_CXX_STANDARD}, ${CMAKE_BUILD_TYPE}).")
