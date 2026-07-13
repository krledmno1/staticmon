cmake_minimum_required(VERSION 3.22)

include(CheckIPOSupported)
project(
  staticmon
  VERSION 0.1
  DESCRIPTION "C++ monitor"
  LANGUAGES C CXX)

option(USE_JEMALLOC "Use jemalloc" OFF)
option(ENABLE_SANITIZERS "Enable ASAN and UBSAN" OFF)
option(ENABLE_FILE_INPUT "Enable reading traces from files" ON)
option(ENABLE_TRACE_TIME "Enable compile time tracing" OFF)
option(ENABLE_SOCK_INTF "Enable socket interface" ON)
option(ENABLE_IPO "Enable interprocedural optimization/LTO" ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Conan v1's conan_basic_setup() used to place binaries in <build>/bin; keep
# that convention (the entrypoint, scripts/staticmon-build and tests rely on it)
# now that dependencies come from Conan 2 instead.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Dependencies come from Conan 2 (CMakeDeps generators); the CMakeToolchain
# file sets CMAKE_PREFIX_PATH so these find_package calls resolve.
find_package(Boost REQUIRED)
find_package(fmt REQUIRED)
find_package(absl REQUIRED)
find_package(jemalloc REQUIRED)

string(REPLACE "-O2" "-O3" CMAKE_CXX_FLAGS_RELWITHDEBINFO
               "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
string(REPLACE "-O2" "-O3" CMAKE_C_FLAGS_RELWITHDEBINFO
               "${CMAKE_C_FLAGS_RELWITHDEBINFO}")

set(IPO_ENABLED FALSE)
if(ENABLE_IPO)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output LANGUAGES C CXX)
  if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    set(IPO_ENABLED TRUE)
  else()
    message(STATUS "IPO disabled: ${ipo_output}")
  endif()
endif()

if(CMAKE_BUILD_TYPE MATCHES DEBUG)
  add_compile_definitions(GLIBCXX_DEBUG=1)
  # add_compile_options(-O2)
endif()

if((CMAKE_BUILD_TYPE MATCHES RELEASE) OR (CMAKE_BUILD_TYPE MATCHES
                                          RELWITHDEBINFO))
  if(IPO_ENABLED)
    add_compile_options(-flto=full)
  endif()
  add_compile_options(-march=native)
endif()

# Treat Apple's clang like upstream clang for compiler-specific flags: CMake
# reports AppleClang as its own COMPILER_ID, so exact "Clang" matches miss it.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang") # matches both "Clang" and "AppleClang"
  set(IS_CLANG_LIKE TRUE)
else()
  set(IS_CLANG_LIKE FALSE)
endif()

# lld is a Linux / upstream-clang concern; macOS links with the system linker.
find_program(TEST_LLD ld.lld)
if(TEST_LLD AND IS_CLANG_LIKE AND NOT APPLE)
  add_link_options(-fuse-ld=lld)
endif()

if(ENABLE_SANITIZERS)
  add_link_options(-fsanitize=address,undefined)
  add_compile_options(-fsanitize=address,undefined)
endif()

# -static-libstdc++ statically links the GNU C++ runtime (Linux). macOS has no
# libstdc++ (Apple clang uses libc++, a system library), so skip it there.
if(NOT APPLE)
  add_link_options(-static-libstdc++)
endif()

if(ENABLE_FILE_INPUT)
  include(cmake/lexy.cmake)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options(-fdiagnostics-color=always -ftemplate-depth=300000)
elseif(IS_CLANG_LIKE)
  add_compile_options(-fcolor-diagnostics)
endif()

add_subdirectory(src)
