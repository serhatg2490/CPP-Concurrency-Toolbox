del /s /q build
cmake -B build -S .
cmake --build build --config Release
ctest --test-dir build -C Release --verbose