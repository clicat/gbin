cmake_minimum_required(VERSION 3.16)

project(gbin_c VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(GBIN_BUILD_TESTS "Build tests" ON)
option(GBIN_BUILD_BENCH "Build benchmarks" ON)
option(GBIN_BUILD_TOOLS "Build CLI tools" ON)

# Prefer a reasonable default build type.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
find_package(ZLIB REQUIRED)
# Needed for the interactive TUI in the CLI (interactive_show)
find_package(Curses REQUIRED)

add_library(gbf STATIC
  src/gbf.c
  src/gbf_json.c
  src/gbf_util.c
  src/gbf_easy.c
)

target_include_directories(gbf
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(gbf PUBLIC ZLIB::ZLIB)

if(NOT MSVC)
  target_link_libraries(gbf PUBLIC m)
endif()

if(MSVC)
  target_compile_options(gbf PRIVATE /W4)
else()
  target_compile_options(gbf PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(GBIN_BUILD_TOOLS)
  add_executable(gbin tools/gbin_cli.c)
  target_link_libraries(gbin PRIVATE gbf ${CURSES_LIBRARIES})
  target_include_directories(gbin PRIVATE ${CURSES_INCLUDE_DIR})
  if(NOT MSVC)
    target_compile_options(gbin PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(GBIN_BUILD_TOOLS)
  add_executable(gbf_client examples/gbf_client.c)
  target_link_libraries(gbf_client PRIVATE gbf)
  if(NOT MSVC)
    target_compile_options(gbf_client PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()

if(GBIN_BUILD_TOOLS)
  add_executable(gbf_easy_demo tools/gbf_easy_demo.c)
  target_link_libraries(gbf_easy_demo PRIVATE gbf)
  if(NOT MSVC)
    target_compile_options(gbf_easy_demo PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()


if(GBIN_BUILD_TESTS)
  enable_testing()
  add_executable(test_gbf tests/test_gbf_roundtrip.c)
  target_link_libraries(test_gbf PRIVATE gbf)
  if(NOT MSVC)
    target_compile_options(test_gbf PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME test_gbf COMMAND test_gbf)
endif()

if(GBIN_BUILD_BENCH)
  add_executable(gbf_bench benches/gbf_bench.c)
  target_link_libraries(gbf_bench PRIVATE gbf)
  if(NOT MSVC)
    target_compile_options(gbf_bench PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endif()