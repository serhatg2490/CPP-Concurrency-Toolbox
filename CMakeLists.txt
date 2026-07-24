cmake_minimum_required(VERSION 3.20)
project(CppConcurrencyToolbox VERSION 1.0.0 LANGUAGES CXX)

# We require C++23 for std::move_only_function
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library. Named after the project, not "ThreadPool" -- the
# library covers the SPSC/SPMC lock-free queues too, not just ThreadPool.h.
add_library(CppConcurrencyToolbox INTERFACE)
target_include_directories(CppConcurrencyToolbox INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Enable testing
enable_testing()

# Include tests subdirectory
add_subdirectory(tests)

# ── Custom latency benchmark (BenchmarkMain.cpp) ──────────────────────────────
add_executable(spmc_benchmark benchmark/BenchmarkMain.cpp)
target_link_libraries(spmc_benchmark PRIVATE CppConcurrencyToolbox)

if(MSVC)
    target_compile_options(spmc_benchmark PRIVATE /O2 /Ob3 /GL /GS-)
    target_link_options   (spmc_benchmark PRIVATE /LTCG)
    target_link_libraries (spmc_benchmark PRIVATE winmm)   # timeBeginPeriod
else()
    target_compile_options(spmc_benchmark PRIVATE -O3 -march=native -fno-stack-protector)
endif()

# ── Google Benchmark integration ──────────────────────────────────────────────
# Downloaded from GitHub on the first cmake configure (~5 MB, shallow clone).
# Subsequent configures use the cached copy.
include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)
# Disable GTest dependency — GB's own tests are not needed
set(BENCHMARK_ENABLE_TESTING        OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS    OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS          OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(spmc_gbenchmark benchmark/GBenchmark.cpp)
target_link_libraries(spmc_gbenchmark PRIVATE CppConcurrencyToolbox benchmark::benchmark benchmark::benchmark_main)

if(MSVC)
    target_compile_options(spmc_gbenchmark PRIVATE /O2 /Ob3 /GS-)
    # /GL + /LTCG can conflict with GB's debug symbols; omitted
else()
    target_compile_options(spmc_gbenchmark PRIVATE -O3 -march=native -fno-stack-protector)
endif()

# ── RDTSCP-based latency benchmark ────────────────────────────────────────────
# Same scenarios as BenchmarkMain.cpp; clock layer: steady_clock -> RDTSCP.
# Works without /arch:AVX2; __rdtscp() only requires <intrin.h>.
add_executable(spmc_tsc_benchmark benchmark/TscBenchmarkMain.cpp)
target_link_libraries(spmc_tsc_benchmark PRIVATE CppConcurrencyToolbox)

if(MSVC)
    target_compile_options(spmc_tsc_benchmark PRIVATE /O2 /Ob3 /GL /GS-)
    target_link_options   (spmc_tsc_benchmark PRIVATE /LTCG)
    target_link_libraries (spmc_tsc_benchmark PRIVATE winmm)   # timeBeginPeriod
else()
    target_compile_options(spmc_tsc_benchmark PRIVATE -O3 -march=native -fno-stack-protector)
endif()

# ── Register benchmarks with CTest ────────────────────────────────────────────
# A plain `ctest` run executes these alongside the unit tests (one command
# builds, tests, and benchmarks). They're labeled "benchmark" so a
# correctness-only pass can skip them:
#   ctest -LE benchmark      # unit tests only, fast
#   ctest -L  benchmark      # benchmarks only
# Root/SCHED_FIFO is not required to run them -- each binary falls back to
# normal scheduling priority (with a warning) when not run as root; see
# docs/benchmark-analysis-2026-07-22.md for why root + core pinning still
# matters for trustworthy latency numbers.
foreach(bench_target spmc_benchmark spmc_tsc_benchmark spmc_gbenchmark)
    add_test(NAME ${bench_target} COMMAND ${bench_target})
    set_tests_properties(${bench_target} PROPERTIES
        LABELS "benchmark"
        TIMEOUT 180
    )
endforeach()
