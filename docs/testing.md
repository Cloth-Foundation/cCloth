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

## Stage 25 enum exit audit

Verified on Windows on 2026-09-02 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- all 95 CTest entries pass in each configuration, including 20 real-compiler
  protocol tests and nine native Shuttle/`clothc` tests;
- enum tests cover always-public case spellings, case-sensitive names, nominal
  package identity and source order, private type access, overloads, required
  initialization and early returns, static constants, fields, arrays, iteration,
  interface calls, and exactly-once output/meta evaluation;
- the parser accepts exactly 65,536 cases and diagnoses the next case, while
  malformed bodies and unsupported operations fail deterministically;
- HIR/MIR/ABI corruption tests reject invalid enum literals, numeric operations,
  missing initialization, and reference representation; imported-view and byte
  codec tests reject wrong tags/owners, duplicate and keyword names, empty case
  sets, forged private-case metadata, invalid static values, and GC metadata;
- format-2 artifacts round-trip, preserve cases with dependency sources removed,
  invalidate consumers after case edits while reusing independent packages, and
  produce byte-identical one-job/four-job package artifacts with native behavior
  equal to whole-project compilation; and
- all 43 ordinary Shuttle tests, Rust formatting, Clippy with warnings denied,
  Rust 1.85 checking, C++ formatting/warnings-as-errors, and both repositories'
  whitespace checks pass.

Repeatable gates are `cmake --build --preset dev`, `ctest --preset dev`,
`cmake --build --preset sanitize`, `ctest --preset sanitize`, and the standalone
Rust commands in `shuttle/docs/testing.md`. Native enum fixtures reuse the
existing integration runner; no additional launch scripts are introduced.

Stage 25 is complete. Artifact format 2 and compiler ABI 3 require rebuilding
older packages; runtime ABI 1 and process protocol 2 are unchanged. Attached
constant data, runtime payload variants, structs, matching, reflection, and
nullable enum values remain deliberate backlog work.

## Stage 26.2 struct frontend checkpoint

Verified on Windows on 2026-09-02 with GNU development and Clang/MSVC-library
ASan/UBSan builds. All **100 CTest entries pass in each configuration**, including
the existing shared Shuttle protocol and native suites. C++ formatting,
warnings-as-errors, and both repositories' whitespace checks pass.

`cloth_struct_tests` covers nominal identity and source order, alias imports,
constructor/member visibility, complete initialization and early exits, final
inline paths, temporary mutation, read-only receivers, mutable reference
boundaries, parameter/iteration bindings, class/interface signatures carrying
structs, operation restrictions, and inline-layout cycles. Negative HIR checks
cover nominal substitutions, location categories, receiver contracts, invalid
equality operations, and scalar aggregate representation. At that checkpoint,
native, ABI, and artifact paths were tested to reject struct compilation.

The then-two-file `tests/integration/projects/structs` fixture exercised `--check`
with both x86-64 and wasm32 selections; separate driver tests rejected native
lowering and conflicting output options. No new testing launch script was needed.

That checkpoint closed **26.2 only**, not Stage 26. Runtime copy/equality/output
behavior, aggregate calls, GC safety, and source-free struct artifacts remained
for 26.3/26.4. Artifact format 2, compiler ABI 3, runtime ABI 1, process protocol
2, and manifest schema 1 were unchanged at that point.

## Stage 26.3 aggregate implementation checkpoint

Verified on Windows on 2026-09-02 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- all **121 CTest entries pass in each configuration**, including 21 real-compiler
  Shuttle protocol tests and ten native Shuttle tests;
- aggregate ABI tests cover x86-64 and wasm32 inline layout, flattened private
  reference maps, physical receiver/parameter/result modes, empty values, and
  limits on fields, nesting, value size, reference maps, and frame storage;
- malformed MIR/ABI/imported metadata is rejected, including incomplete
  construction, repeated final initialization, invalid storage roots, wrong
  calling modes, false maps, inline cycles, and forged dependency layouts;
- format-3 interface artifacts have frozen complete-byte lengths and SHA-256
  hashes for both targets, with byte-identical re-encoding and source-free
  private-layout consumption;
- native fixtures cover the `Data(uint64)` constructor/getter, independent
  copies, shallow managed references, nested mutation, exactly-once location
  evaluation, readonly receiver and argument snapshots, fieldwise equality
  including NaN and nullable strings, output/meta, and class/interface calls;
- a test-only native harness inserts collection before allocation and exercises
  loop-carried aggregate phi copies; constructor, local, argument, result, array,
  and captured-owner references survive those safepoints;
- runtime tests cover multi-reference array elements, interior root slots, and
  malformed metadata/overflow traps through the existing program runner;
- Shuttle builds native and wasm32 aggregate dependencies without reopening
  their sources; separate native output matches whole-project output, and old
  format-2 capabilities/receipts are rejected; and
- all 43 ordinary Shuttle tests, Rust formatting, Clippy with warnings denied,
  Rust 1.85 checking, C++ formatting/warnings-as-errors, and both repositories'
  whitespace checks pass.

Frontend/HIR checks remain in `cloth_struct_tests`; layout/package/MIR checks
live in `cloth_aggregate_tests`. The native stress harness uses the existing
integration runner and production verification/emission pipeline rather than
adding a runtime GC-testing API or a new launch script.

That checkpoint closed **26.3 only**. Artifact format 3, compiler ABI 4 (`_C4`),
and runtime ABI 2 require rebuilding older packages. Process protocol 2, receipt schema 1,
and manifest schema 1 remain unchanged. The remaining coordinated exit work was
verified in 26.4 below.

## Stage 26.4 struct exit audit

Verified on Windows on 2026-09-02 with GNU development and Clang/MSVC-library
ASan/UBSan builds. All **121 CTest entries pass in each configuration**, including
all **22 shared compiler-protocol tests and 12 native Shuttle tests**. The CTest
entry count is unchanged because coverage was added to the existing suites.

- Native and forced-GC fixtures now also exercise constructor overloads and
  early returns, struct-parameter overload resolution, inherited/interface
  dispatch and `super` calls carrying values, inherited aggregate fields,
  final/reference mutation boundaries, private-field equality, arrays of empty
  structs, and exactly-once output/meta evaluation. The same sources pass LLVM
  verification for x86-64 and wasm32.
- Aggregate MIR corruption tests reject missing, duplicate, and non-predecessor
  phi edges, scalar inputs to aggregate phis, and phis after ordinary
  instructions. Existing HIR, initialization, ABI, map, cycle, and resource
  boundary regressions remain green.
- Relocated `--jobs 1` and `--jobs 4` struct builds produce byte-identical
  interface artifacts on both targets and byte-identical native artifacts and
  executables on x86-64, including across a PE timestamp boundary. Native output
  matches whole-project compilation before and after dependency edits.
- Adding a private struct field changes layout/reference offsets; adding a
  member changes the declaration contract. Both edits rebuild `data-models` and
  its consumer while reusing byte-identical `foundation` and `tools` artifacts.
  Subsequent unchanged builds reuse all four packages. These checks run for
  interface artifacts on both targets and native objects/execution on x86-64.
- Source-free consumers retain private layouts, overloads, aggregate constructor
  arguments/results, inherited fields, and interface/base calls. Private
  constructors, fields, and methods remain inaccessible after dependency sources
  are removed; failed consumer compilation preserves its previous artifact.
- The audit found and fixed one diagnostic-contract mismatch: aggregate resource
  violations were rejected but classified as invalid models/malformed metadata.
  Typed limit issues now survive imported-model and artifact validation, and
  value/descriptor map counts are checked before constructing decoded offset
  vectors. Re-signed oversized-map artifacts and oversized writer models cover
  the fix without changing the existing golden bytes or compatibility versions.

All 43 ordinary Shuttle tests, Rust 1.85 checking, Rust formatting, Clippy with
warnings denied, C++ formatting/warnings-as-errors, and both repositories'
whitespace checks pass. Existing fixtures and runners were extended; no new
test launch scripts or production testing switches were introduced.

**Stage 26 is complete.** Artifact format 3, compiler ABI 4, runtime ABI 2,
process protocol 2, receipt schema 1, and manifest schema 1 are unchanged by
26.4. Mutating receivers, struct conformance/boxing, reference returns, nullable
struct values, aggregate constants, and other listed non-goals remain deferred.
No later stage is implicitly activated.

## Stage 26.5.1 explicit interface overrides

Verified on Windows on 2026-09-02: **122/122 development and 122/122 ASan/UBSan
CTests pass**, including 22 shared protocol and 12 native Shuttle tests. All 43
ordinary Rust tests, Rust 1.85 checking, formatting, Clippy with warnings denied,
and C++ formatting/warnings-as-errors pass.

The focused override suite covers direct/transitive and multiple contracts,
overloads, abstract restatements, final sealing, inherited implementation reuse,
class/interface targets together, covariance, invalid modifiers/signatures, and
base-only `super`. Reversed source registration exposed a covariance ordering
bug: class interface closures are now completed before base-first override
validation. Both source orders pass.

Imported-package tests reject missing/spurious markers and forged replaced-class
identities, while accepting interface-only overrides without a base target.
Shared protocol tests remove dependency sources, reject missing/unmatched
markers without replacing completed output, and accept the corrected consumer.
Existing native/equivalence fixtures retain class and interface dispatch.

VS Code contributes the existing `override` modifier and an editable function
snippet. All three Node support tests pass with each compiler, including actual
compilation of the snippet and rejection after removing its marker. On Windows,
the sanitizer run needs the Clang ASan runtime directory on `PATH`, as in CTest.
Legacy new-file generator alignment remains explicitly deferred in `TODO.md`.

**26.5.1 is complete.** Existing interface implementations must add `override`;
inherited implementations need no redeclaration. No `impl` keyword, artifact
schema, physical ABI, process protocol, or manifest revision was introduced.
