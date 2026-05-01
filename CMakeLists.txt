cmake_minimum_required(VERSION 3.20)
project(ThreadPool VERSION 1.0.0 LANGUAGES CXX)

# We require C++23 for std::move_only_function
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Header-only library
add_library(ThreadPool INTERFACE)
target_include_directories(ThreadPool INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Enable testing
enable_testing()

# Include tests subdirectory
add_subdirectory(tests)
