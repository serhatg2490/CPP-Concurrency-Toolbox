@echo off
REM Run tests (and, by default, benchmarks) against an existing build.
REM Run build.bat first if build\ doesn't exist yet.
REM
REM Usage:
REM   test.bat              :: unit tests + benchmarks (default)
REM   test.bat --unit-only  :: unit tests only, skips the benchmark run

if not exist build\CMakeCache.txt (
    echo No build\ found -- run build.bat first. 1>&2
    exit /b 1
)

if "%~1"=="--unit-only" (
    ctest --test-dir build -C Release --verbose -LE benchmark
) else (
    ctest --test-dir build -C Release --verbose
)
