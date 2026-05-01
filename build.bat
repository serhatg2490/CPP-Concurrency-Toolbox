del /s /q build
cmake -B build -S .
cmake --build build
ctest --test-dir build --verbose