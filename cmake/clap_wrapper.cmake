include(FetchContent)

# Pinned tag for reproducible builds. Bump deliberately.
set(PROBE_CLAP_WRAPPER_TAG "v0.14.0" CACHE STRING
  "free-audio/clap-wrapper tag to fetch")

set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_BUILD_AUV2 ${APPLE} CACHE BOOL "" FORCE)
set(CLAP_WRAPPER_DONT_ADD_TARGETS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  clap_wrapper
  GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
  GIT_TAG ${PROBE_CLAP_WRAPPER_TAG}
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(clap_wrapper)
