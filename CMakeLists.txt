cmake_minimum_required(VERSION 3.20)
project(lob_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_BUILD_TESTS "Build unit tests" ON)
option(LOB_BUILD_BENCHMARKS "Build benchmarks" ON)
option(LOB_BUILD_EXAMPLES "Build examples" ON)
option(LOB_BUILD_TOOLS "Build market-data tools" ON)
option(LOB_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

# ---- Library (header-only) -------------------------------------------------
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(lob INTERFACE cxx_std_20)

# ---- Shared compile flags for our own targets ------------------------------
add_library(lob_flags INTERFACE)
if(MSVC)
  target_compile_options(lob_flags INTERFACE /W4 /permissive-)
else()
  target_compile_options(lob_flags INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                             -Wsign-conversion)
  if(LOB_SANITIZE)
    target_compile_options(lob_flags INTERFACE -fsanitize=address,undefined
                                               -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(lob_flags INTERFACE -fsanitize=address,undefined)
  endif()
endif()

include(FetchContent)

# ---- Tests ------------------------------------------------------------------
if(LOB_BUILD_TESTS)
  enable_testing()
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(googletest)

  add_executable(lob_tests tests/test_order_book.cpp tests/test_reference_model.cpp
                         tests/test_replay.cpp tests/test_sim.cpp)
  target_link_libraries(lob_tests PRIVATE lob::lob lob_flags GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()

# ---- Benchmarks -------------------------------------------------------------
if(LOB_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(benchmark
    URL https://github.com/google/benchmark/archive/refs/tags/v1.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(benchmark)

  add_executable(lob_bench bench/bench_order_book.cpp)
  target_link_libraries(lob_bench PRIVATE lob::lob lob_flags benchmark::benchmark)

  add_executable(lob_latency bench/latency.cpp)
  target_link_libraries(lob_latency PRIVATE lob::lob lob_flags)
endif()

# ---- Examples ---------------------------------------------------------------
if(LOB_BUILD_EXAMPLES)
  add_executable(basic_usage examples/basic_usage.cpp)
  target_link_libraries(basic_usage PRIVATE lob::lob lob_flags)
endif()

# ---- Tools ------------------------------------------------------------------
if(LOB_BUILD_TOOLS)
  add_executable(lob_replay tools/replay.cpp)
  target_link_libraries(lob_replay PRIVATE lob::lob lob_flags)

  add_executable(lob_mm_backtest tools/mm_backtest.cpp)
  target_link_libraries(lob_mm_backtest PRIVATE lob::lob lob_flags)
endif()
