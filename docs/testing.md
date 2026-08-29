# Cloth testing and diagnostic builds

Stage 10.1 treats tests as executable language contracts. The default suite
contains unit, verifier, LLVM, driver, native-output, and expected-failure
tests. Run it after building:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Each CTest case defaults to a 15-second timeout. Native programs launched by a
test script default to 10 seconds, preventing a broken loop backedge from
hanging a development or CI run. Configure `CLOTH_TEST_TIMEOUT_SECONDS` to
change the outer limit.

Test ownership follows the behavior being exercised:

- `tests/unit/` contains C++ unit and verifier executables.
- `tests/integration/programs/` contains standalone Cloth programs.
- `tests/integration/projects/` contains complete multi-file projects.
- `tests/integration/errors/` contains compiler-failure inputs.
- `tests/integration/expected/` contains native-output golden files.
- `tests/fuzz/` contains opt-in fuzz targets and curated source seeds.

The folder-local `CMakeLists.txt` files own their targets. Native output and
runtime-failure cases share `tests/integration/RunProgram.cmake`; test-specific
launch scripts should not be added. CTest labels allow focused runs with
`ctest --preset dev -L unit` or `ctest --preset dev -L integration`.

## Sanitizers

AddressSanitizer and, where supported, UndefinedBehaviorSanitizer are opt-in
and cannot be mixed with coverage instrumentation. CMake verifies that the
selected compiler has the required runtimes before generating the build:

```sh
cmake --preset sanitize -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset sanitize
ctest --preset sanitize
```

Compiler and test targets are instrumented. `cloth_runtime` remains
uninstrumented because it is linked into independently generated native Cloth
executables; those programs remain covered by the native golden and failure
tests. Windows Clang runtime discovery and test environment setup are handled
by CMake.

## Coverage

GNU and Clang coverage instrumentation is available without changing normal
builds:

```sh
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage
```

The build produces the compiler's native coverage data for tools such as
`gcov` or `gcovr`. Runtime instrumentation is excluded for the same native-link
boundary described above.

## Lexer and parser fuzzing

The libFuzzer target requires Clang and sanitizer mode. Seed inputs include
valid and malformed array iteration:

```sh
cmake --preset fuzz -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset fuzz
ctest --preset fuzz
```

For a longer local campaign, invoke `cloth_lexer_parser_fuzzer` with the copied
build corpus at `build/fuzz/tests/fuzz/corpus/lexer_parser` and an explicit time
or run limit. The source tree retains only curated `.co` seeds; hash-named
generated corpus entries stay in the ignored build directory. The target feeds
arbitrary bytes through both lexer and two-pass parser; diagnostics and invalid
input must never crash or violate sanitizer checks.

The presets select build behavior but do not hard-code a compiler. Pass a
compiler on the first configure, as shown for Clang above, or select one through
the environment or a CMake toolchain file.
