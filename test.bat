@echo off
REM Run tests (and, by default, benchmarks) against an existing build.
REM Run build.bat first if build\ doesn't exist yet.
REM
REM Usage:
REM   test.bat              :: unit tests (quiet unless failing) + benchmarks (full output)
REM   test.bat --unit-only  :: unit tests only, skips the benchmark run

if not exist build\CMakeCache.txt (
    echo No build\ found -- run build.bat first. 1>&2
    exit /b 1
)

REM Unit tests: quiet unless something fails.
ctest --test-dir build -C Release --output-on-failure -LE benchmark
if errorlevel 1 exit /b 1

if not "%~1"=="--unit-only" (
    REM Benchmarks: always show full output -- the whole point of running
    REM them is to see the latency numbers, not just confirm they didn't crash.
    ctest --test-dir build -C Release --verbose -L benchmark
)
