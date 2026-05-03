include(FetchContent)
FetchContent_Declare(
  choc
  GIT_REPOSITORY https://github.com/Tracktion/choc.git
  GIT_TAG main
  GIT_SHALLOW TRUE
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(choc)
