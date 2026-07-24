@echo off
REM Build only (see test.bat to run unit tests / benchmarks afterward).
REM
REM Usage:
REM   build.bat            :: configure (if needed) + incremental build
REM   build.bat --clean    :: remove build\ first, then configure + build from scratch

if "%~1"=="--clean" (
    if exist build rmdir /s /q build
)

if not exist build\CMakeCache.txt (
    cmake -B build -S .
)

cmake --build build --config Release
