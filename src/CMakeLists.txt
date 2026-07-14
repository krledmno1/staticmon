configure_file(config.h.in config.h @ONLY)

set(STATICMON_SOURCES staticmon.cpp)
add_executable(staticmon ${STATICMON_SOURCES})
target_include_directories(staticmon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                             ${CMAKE_CURRENT_BINARY_DIR})
if(USE_JEMALLOC)
  target_link_libraries(staticmon jemalloc::jemalloc)
endif()

target_link_libraries(staticmon boost::boost fmt::fmt
                      abseil::abseil foonathan::lexy)
target_compile_options(staticmon PRIVATE -Wall -Wextra -Wconversion
                                         -Wsign-conversion)
if(ENABLE_TRACE_TIME)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang") # "Clang" or "AppleClang"
    target_compile_options(staticmon PRIVATE -ftime-trace)
  else()
    message(FATAL_ERROR "tracing only works with Clang")
  endif()
endif()

# Standalone formula/signature parser tool (no external dependencies); used
# by the differential test harness in test/parser_diff.
add_executable(parser_dump tools/parser_dump.cpp)
target_include_directories(parser_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(parser_dump PRIVATE -Wall -Wextra -Wconversion
                                           -Wsign-conversion)

# Native -explicitmon replacement: parse+desugar+translate+codegen -> headers.
add_executable(staticmon-headers tools/staticmon-headers.cpp)
target_include_directories(staticmon-headers PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(staticmon-headers PRIVATE -Wall -Wextra)
