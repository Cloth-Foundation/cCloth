# Cloth testing and diagnostic builds

Tests are executable language and toolchain contracts. The default suite
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

The shared Shuttle suites have a separate 1,200-second CTest limit, including
Cargo startup and compilation; each child process inside them has a 300-second
limit. CTest serializes their Cargo access even under parallel test execution.

Test ownership follows the behavior being exercised:

- `tests/unit/` contains C++ unit and verifier executables.
- `tests/integration/programs/` contains standalone Cloth programs.
- `tests/integration/projects/` contains complete multi-file projects.
- `tests/integration/errors/` contains compiler-failure inputs.
- `tests/integration/expected/` contains native-output golden files.
- `tests/fuzz/` contains opt-in fuzz targets and curated source seeds.
- `shuttle/tests/` owns the shared multi-package fixtures and process-protocol
  runner. CTest invokes it against the compiler from the current build tree.

The folder-local `CMakeLists.txt` files own their targets. Native output and
runtime-failure cases share `tests/integration/RunProgram.cmake`; test-specific
launch scripts should not be added. CTest labels allow focused runs with
`ctest --preset dev -L unit` or `ctest --preset dev -L integration`.

## Shuttle and compiler boundary

The development and sanitizer presets enable `CLOTH_TEST_SHUTTLE`. Initialize
the `shuttle` submodule and install Rust 1.85 or newer with Cargo. A missing
checkout or Cargo executable is a configure error rather than a skipped suite.
`CLOTH_SHUTTLE_SOURCE_DIR` may point to another compatible Shuttle checkout.

```sh
ctest --preset dev -L toolchain
ctest --preset sanitize -L toolchain
```

The protocol suite always runs when enabled. Native execution tests are also
registered when LLVM `llc` is available, using the compiler's configured linker
and runtime. A complete toolchain exit audit requires those native tests.
For deliberate compiler-only work, use `-DCLOTH_TEST_SHUTTLE=OFF`; that is not a
complete cross-tool audit.

The shared fixture is a four-package dependency diamond. Tests cover owner-local
aliases, transitive isolation, visibility, equal relative type names, exact
entry selection, stream/status forwarding, malformed protocol requests,
Unicode and space-containing paths, artifact preservation, and relocated IR
and native-byte reproducibility. Tests copy fixtures into temporary directories;
they never build inside the checked-in fixture tree.

Shuttle's ordinary unit and process tests are independent of `clothc`:

```sh
cargo test --manifest-path shuttle/Cargo.toml --all-targets --locked
cargo clippy --manifest-path shuttle/Cargo.toml --all-targets --locked -- -D warnings
cargo fmt --manifest-path shuttle/Cargo.toml --all -- --check
cargo +1.85.0 check --manifest-path shuttle/Cargo.toml --all-targets --locked
```

The real-compiler cases are marked ignored in standalone Cargo runs. CTest
explicitly runs them with `--ignored` and an absolute `CLOTHC_UNDER_TEST` path;
they fail if that path is missing or invalid. See
[Shuttle testing](../shuttle/docs/testing.md) for standalone invocation.

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

The shared suites launch the instrumented compiler, so protocol parsing,
package compilation, Unicode handling, and entry selection are covered by the
same sanitizer gate. Shuttle itself stays on stable Rust with unsafe code
forbidden; this audit does not claim nightly Rust sanitizer instrumentation.

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

## Stage 22 exit audit

Verified on Windows on 2026-08-31 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- 89/89 CTest entries pass in each configuration, including all 17 shared
  protocol/native cases;
- all 34 standalone Shuttle tests pass;
- C++ formatting and warnings-as-errors, Rust formatting, Clippy with warnings
  denied, and the Rust 1.85 MSRV check pass;
- relocated native builds are byte-identical in both linker configurations;
- direct compilation works without reading either manifest filename; and
- malformed configuration fails before compiler invocation, while source
  diagnostics and supported exit statuses cross the process boundary intact.

The Unix-only symbolic-link case remains platform-conditioned and was not run
on this Windows host. Remote dependencies, separate artifacts, caching, and
additional native targets remain outside Stage 22's approved scope.

## Stage 23.2 exit audit

Verified on Windows on 2026-08-31 with the GNU development compiler and the
Clang ASan/UBSan compiler:

- 92/92 CTest entries pass in each configuration, including the 17 shared
  Shuttle protocol/native cases;
- the new identity suite fixes encoded byte/hash vectors and checks aliases,
  relocation, source order, exact versions, domain separation, nullability,
  package validation, and corrupted descriptor/initializer ABI metadata;
- the imported-package suite verifies detached ownership, private declaration
  retention, reachable type closure, nullable overload identity, static literal
  values, inheritance/interface layouts, constructor initializer signatures,
  canonical ordering, and corrupted cross-record rejection;
- the package-artifact suite fixes SHA-256 and canonical schema vectors, verifies
  byte-identical interface/object round trips and exact target compatibility,
  and rejects oversized, truncated, corrupted, noncanonical, structurally
  invalid, or wrong-machine artifacts before they become imported views; and
- existing native inheritance, interface, GC, entry, and deterministic-output
  tests pass with ABI-2 names and external descriptor/initializer ownership.

Both builds use warnings as errors; the C++ `check_format` target and both
repositories' `git diff --check` also pass.

Package-scoped LLVM unit coverage checks owning definitions, external
dependency declarations, private-body exclusion, and rejected entry options.
It uses a verified whole-project graph. At the Stage 23.2 checkpoint,
protocol-v2 orchestration and separate native linking remained outstanding;
the next audit records that pipeline work.

## Stage 23 exit audit

Verified on Windows on 2026-09-01 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- 92/92 CTest entries pass in each configuration, including all 14 protocol
  and seven native Shuttle/`clothc` cases;
- whole-project and separate compilation produce equal imported semantic/ABI
  records and canonical linker-symbol ownership for the covered package graph;
- the native equivalence graph covers cross-package construction, inherited
  fields, `super`, abstract and covariant functions, final overrides,
  interfaces, checked casts and `::typeName`, nullable flow, arrays, strings,
  and a 2,000-object cross-package GC chain with identical output and status;
- consumer packages compile from validated artifacts after every dependency
  source tree is removed, while rejected programs retain the same diagnostic
  categories and use logical artifact declaration locations;
- missing closures, wrong identities, wrong digests, corrupted payloads,
  duplicate artifact inputs, and output/input aliasing fail without replacing
  prior artifacts or executables;
- relocated native builds produce byte-identical executables and package
  artifacts from identical inputs, and both `x86_64` and `wasm32` interface
  paths remain covered; and
- all 36 ordinary Shuttle tests, Rust 1.85 checking, Rust formatting and Clippy
  with warnings denied, C++ formatting and warnings-as-errors, and repository
  whitespace checks pass.

Stage 23 is complete. Automatic reuse across commands, remote artifacts,
registries, additional native targets, and ABI stability across compiler
releases remain deliberately outside its contract.

## Stage 24.2 responsiveness checkpoint

Verified on Windows on 2026-09-01 with release Shuttle and the GNU development
compiler. The repeatable one-package `examples/Shuttle.toml` `wasm32` check fell
from an initial 9.35-second observation to a 161.9-millisecond median across
five warm-file-system runs. The compiler capability query within that path fell
from 5.86 seconds to a 70.2-millisecond median. This is a 98.3% end-to-end
reduction without changing exact artifact or tool identity.

Shuttle now reports preparation, every package, linking, completion time, and
program launch on standard error by default; `--quiet` suppresses successful
progress without suppressing compiler diagnostics or program streams. All
92 development and 92 sanitizer CTest entries pass, along with all 37 ordinary
Shuttle tests, the Rust 1.85 baseline, Rust formatting and Clippy, C++ formatting,
and repository whitespace checks. At this checkpoint, Stage 24.1 and 24.2 were
complete; validated reuse and deterministic parallel scheduling remained active.

## Stage 24.3 reuse checkpoint

Verified on Windows on 2026-09-01 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler. Protocol version 2 now validates reuse
against the exact current source inventory, dependency artifact closure,
compiler, target, runtime, and native tools before returning a receipt. A clean
status-3 miss has empty streams and is the only path from validation to
compilation.

Shuttle persists immutable, atomically published manifest/receipt records and
requires both an exact manifest snapshot and a matching compiler receipt.
Tests cover unchanged interface/object graphs, manifest-only invalidation,
source and dependency invalidation, target isolation, changed compilers,
runtime/tool compatibility, malformed state, corrupt candidate repair, entry
failures, and concurrent interface/object writers.

All 92 development and 92 sanitizer CTest entries pass, including all 17
protocol and eight native Shuttle/`clothc` cases. All 40 ordinary Shuttle tests,
the Rust 1.85 baseline, Rust formatting and Clippy, C++ formatting, and
repository whitespace checks pass. Stage 24.3 is complete; bounded deterministic
parallel scheduling remains Stage 24.4.

## Stage 24.4 parallel scheduling and exit audit

Verified on Windows on 2026-09-01 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler. Shuttle now bounds package compiler
processes with a positive `--jobs` value, defaults to available host parallelism,
and schedules canonical dependency levels. Candidate validation and compilation
remain separate phases, so every long compiler operation has preceding progress.

Compiler diagnostics are drained into private per-process spools and replayed
unchanged in canonical order. A process barrier proves that the independent
fixture packages overlap with two jobs, while reversed wall-clock failures still
select the exact `--jobs 1` diagnostic. Real-compiler diagnostics are byte-equal
between serial and parallel checks. Relocated one-job and four-job native builds
produce byte-identical package artifacts and executables.

All 92 development and 92 sanitizer CTest entries pass, including all 18
protocol and eight native Shuttle/`clothc` cases. All 43 ordinary Shuttle tests,
the Rust 1.85 baseline, Rust formatting and Clippy with warnings denied, C++
formatting, and both repositories' whitespace checks pass. The Stage 24.2
responsiveness baseline and Stage 24.3 unchanged-build coverage remain intact.
Stage 24 is complete.
