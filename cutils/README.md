# cutils

Requires CMake 3.28+, GCC 16 (`g++-16`), and Python 3. Testing fetches Catch2 v3.16.0 (BSL-1.0).

From the repository root:

```sh
cmake -S cutils -B build/cutils -DCMAKE_CXX_COMPILER=g++-16 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/cutils -j
ctest --test-dir build/cutils --output-on-failure
```

Tests require sanitizers, with ASan and UBSan as the default.

For separate TSan race checks, use a new build directory, Clang, and
`-DCUTILS_TEST_TSAN=ON`. Clang runs the non-reflection tests only.
