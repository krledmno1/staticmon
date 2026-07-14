include(FetchContent)

FetchContent_Declare(lexy
    GIT_REPOSITORY https://github.com/foonathan/lexy.git
    GIT_TAG v2025.05.0)

# We only consume lexy as a build dependency; don't drag its headers/libs into
# our `cmake --install` prefix.
set(LEXY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lexy)
