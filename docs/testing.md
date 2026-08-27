# Cloth testing and diagnostic builds

Stage 10.1 treats tests as executable language contracts. The default suite
contains unit, verifier, LLVM, driver, native-output, and expected-failure
tests. Run it after building:

```sh
ctest --test-dir build --output-on-failure
```

Each CTest case defaults to a 15-second timeout. Native programs launched by a
test script default to 10 seconds, preventing a broken loop backedge from
hanging a development or CI run. Configure `CLOTH_TEST_TIMEOUT_SECONDS` to
change the outer limit.

## Sanitizers

AddressSanitizer and, where supported, UndefinedBehaviorSanitizer are opt-in
and cannot be mixed with coverage instrumentation. CMake verifies that the
selected compiler has the required runtimes before generating the build:

```sh
cmake -S . -B build-sanitize \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCLOTH_ENABLE_SANITIZERS=ON \
  -DCLOTH_WARNINGS_AS_ERRORS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
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
cmake -S . -B build-coverage \
  -DCLOTH_ENABLE_COVERAGE=ON \
  -DCLOTH_WARNINGS_AS_ERRORS=ON
cmake --build build-coverage
ctest --test-dir build-coverage --output-on-failure
```

The build produces the compiler's native coverage data for tools such as
`gcov` or `gcovr`. Runtime instrumentation is excluded for the same native-link
boundary described above.

## Lexer and parser fuzzing

The libFuzzer target requires Clang and sanitizer mode. Seed inputs include
valid and malformed array iteration:

```sh
cmake -S . -B build-fuzz \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DCLOTH_ENABLE_SANITIZERS=ON \
  -DCLOTH_BUILD_FUZZERS=ON
cmake --build build-fuzz --target cloth_lexer_parser_fuzzer
ctest --test-dir build-fuzz -R cloth_lexer_parser_fuzzer_smoke \
  --output-on-failure
```

For a longer local campaign, invoke `cloth_lexer_parser_fuzzer` directly with
`tests/fuzz_corpus/lexer_parser` and an explicit time or run limit. The target
feeds arbitrary bytes through both lexer and two-pass parser; diagnostics and
invalid input must never crash or violate sanitizer checks.
