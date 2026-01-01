cmake_minimum_required(VERSION 3.16)
project(gbin_cpp LANGUAGES CXX)

option(GBIN_BUILD_TESTS "Build GBF tests" ON)
option(GBIN_BUILD_BENCH "Build GBF benchmarks" ON)
option(GBIN_BUILD_CLI   "Build GBF CLI tool" ON)
option(GBIN_BUILD_DEMO  "Build GBF demo program" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---- ZLIB ----
find_package(ZLIB REQUIRED)

# ---- Core library (NO TUI dependencies) ----
add_library(gbin STATIC
    src/gbf.cpp
)

target_include_directories(gbin PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(gbin PUBLIC ZLIB::ZLIB)

# ---- CLI (FTXUI) ----
if (GBIN_BUILD_CLI)
    # FTXUI is only required for the CLI
    FetchContent_Declare(
      ftxui
      GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
      GIT_TAG v5.0.0
    )
    FetchContent_MakeAvailable(ftxui)

    add_executable(gbin_cli tools/gbin_cli.cpp)
    set_target_properties(gbin_cli PROPERTIES OUTPUT_NAME gbin)

    target_link_libraries(gbin_cli PRIVATE gbin ftxui::component)
endif()

# ---- Demo ----
if (GBIN_BUILD_DEMO)
    add_executable(gbf_demo tools/gbf_demo.cpp)
    target_link_libraries(gbf_demo PRIVATE gbin)
endif()

# ---- Tests ----
if (GBIN_BUILD_TESTS)
    enable_testing()
    add_executable(test_gbf tests/test_gbf_roundtrip.cpp)
    target_link_libraries(test_gbf PRIVATE gbin)
    add_test(NAME test_gbf COMMAND test_gbf)
endif()

# ---- Bench ----
if (GBIN_BUILD_BENCH)
    add_executable(gbf_bench benches/gbf_bench.cpp)
    target_link_libraries(gbf_bench PRIVATE gbin)
endif()