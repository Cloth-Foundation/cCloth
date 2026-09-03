# Cloth work ledger

`ROADMAP.md` defines stage order and scope. This file tracks the concrete work
inside scheduled stages and preserves accepted but unscheduled gaps. Implemented
user behavior belongs in `documentation/`; compiler contracts and maintainer
guidance belong in `docs/`, rather than in a growing history of checked boxes.

Rules:

- only work under the active stage may be implemented as a feature;
- a planned stage is not active until its design and implementation start are
  explicitly approved;
- new ideas enter the unscheduled backlog unless the roadmap is changed first;
- completing an item requires implementation, verification, tests, and updates
  to its owning contract; and
- moving or splitting an item must preserve its prerequisites and non-goals.

## Stage status

Stage 22 is complete, including the cross-tool exit audit in `docs/testing.md`.
Stage 23 is also complete, including its coordinated development and sanitizer
exit audit. Stage 24 is complete under the responsive-build charter in
`ROADMAP.md`. The ownership boundary remains recorded in
`docs/shuttle_and_compiler.md`.

Stage 25 is complete for named value enums. Its
[25.1 contract](docs/proposals/stage_25_enums.md) and implementation start were
approved on 2026-09-02, followed by the coordinated exit audit in `docs/testing.md`.
Attached constant data and runtime payloads remain deferred.

Stage 26 is complete for value structs. The
[26.1 source contract](docs/proposals/stage_26_structs.md), including read-only
value receivers, and 26.2 implementation were approved on 2026-09-02.
The frontend and [26.3 aggregate implementation](docs/proposals/stage_26_aggregate_abi.md)
are complete, including native execution and source-free packages. Artifact
format 3, compiler ABI 4, and runtime ABI 2 require rebuilding older packages.
The [26.4 exit audit](docs/testing.md#stage-264-struct-exit-audit) passed on
2026-09-02. Stage 26.5.1 is complete for the approved explicit interface-override
contract; its exit audit is recorded in `docs/testing.md`. Unrelated ideas
remain in the backlog.

Stage 27 is complete. Its [switch contract](docs/proposals/stage_27_switch.md)
and implementation through 27.4 were approved on 2026-09-02. Frontend, native
lowering, and the coordinated [exit audit](docs/testing.md#stage-274-switch-exit-audit)
are complete. No later stage is activated.

## Scheduled work

### Stage 21: Integer binary representation and byte order

- [x] **21.1 — Integer operator contract.** Freeze valid operand types, result
  types, precedence, signed complement, signed right shift, shift-count bounds,
  compound assignment, constant behavior, and diagnostics for `&`, `|`, `^`,
  `~`, `<<`, and `>>`.
- [x] **21.2 — Integer operator implementation.** Carry the approved contract
  through parser/AST, semantic analysis, HIR, MIR verification, LLVM lowering,
  invalid-program coverage, and native execution tests.
- [x] **21.3 — Byte-order contract.** Approve the source surface and semantics
  for explicit little-endian and big-endian encoding and decoding of fixed-width
  integers. Specify byte-array size, offset, bounds, signedness, evaluation
  order, and failure behavior without exposing host-native memory.
- [x] **21.4 — Byte-order implementation.** Implement the approved surface,
  retain the operation explicitly through verified compiler representations,
  and test identical byte sequences independent of target layout metadata.
- [x] Complete the Stage 21 exit audit in `ROADMAP.md`, including development
  and sanitizer suites and recording every deliberate deferral below.

### Stage 22: Shuttle project contract and compiler build protocol

- [x] **22.1 — Manifest and build-protocol contract.** Freeze Shuttle's Rust
  2024 implementation policy, `Shuttle.toml`, package/workspace terminology,
  dependency namespace mapping, the versioned Shuttle-to-compiler request,
  migration from `cloth.toml`, and diagnostics ownership. The approved contracts
  live under `shuttle/docs/` and `docs/shuttle_and_compiler.md`.
- [x] **22.2 — Shuttle bootstrap.** Establish the Rust application, production
  CLI and diagnostics boundaries, manifest model and parser, deterministic
  validation, formatting, linting, and unit-test harness.
- [x] **22.3 — Local graph and compiler integration.** Resolve local
  dependencies deterministically in Shuttle; add the approved package graph,
  source-root, dependency, entry, target, and output inputs to `clothc`; remove
  compiler manifest discovery; and preserve standalone compilation and
  identifier-based imports.
- [x] **22.4 — Cross-tool verification.** Add configuration unit tests,
  multi-project fixtures spanning both repositories, direct and Shuttle build
  documentation, and the Stage 22 development/sanitizer exit audit.

### Stage 23: Shuttle-orchestrated separate compilation and linking

- [x] **23.1 — Artifact contract.** Define the versioned package artifact and
  the semantic/ABI, native payload, and dependency metadata it owns. Approve
  the [artifact proposal](docs/proposals/stage_23_artifacts.md) and
  [process-v2 proposal](shuttle/docs/proposals/compiler_protocol_v2.md), including
  check-only artifacts, compatibility, validation, and deliberate non-goals.
- [x] **23.2 — Canonical identity.** Preserve type descriptors, interface
  identities, mangled callables, constructor initializer linkage, and runtime
  ownership across artifacts. Implement imported declaration/ABI views and
  bounded artifact serialization/verification with canonical schema fixtures;
  do not serialize MIR or dependency bodies.

  - [x] Retain exact package versions and implement canonical type/member
    identities with fixed encoding/hash fixtures and order/alias/path tests.
  - [x] Give descriptors canonical ownership, expose accessible constructor
    initializers, and add a package-scoped LLVM definition/declaration boundary.
  - [x] Build owned, verified imported declaration/ABI views without synthetic
    source ASTs or dependency bodies.
  - [x] Freeze artifact record-schema fixtures and implement bounded canonical
    serialization, integrity/compatibility validation, and malformed-input tests.

  Stage 23.2 is documented in [canonical identity](docs/canonical_identity.md),
  [imported package views](docs/imported_package_views.md), and the
  [artifact schema](docs/artifact_schema_v1.md). Protocol-v2 compilation,
  dependency closure, and linking are connected by Stage 23.3.

- [x] **23.3 — Link pipeline.** Make Shuttle order and invoke deterministic
  compilation and linking while the compiler diagnoses duplicate, missing, or
  incompatible artifact definitions.
- [x] **23.4 — Equivalence verification.** Compare separate and whole-project
  compilation in ABI, linker, invalid-input, and native execution tests, then
  complete the Stage 23 development/sanitizer exit audit.

### Stage 24: Responsive and observable local builds

- [x] **24.1 — Baseline and progress contract.** Record clean-build phase
  timings and define stable package, link, completion, and run progress on
  standard error without changing compiler diagnostics or program streams.
- [x] **24.2 — Cold-path efficiency.** Optimize the measured exact-identity and
  process overhead while preserving Stage 23 compatibility and validation.
- [x] **24.3 — Validated local reuse.** Persist conservative Shuttle-owned
  package input state, reuse only integrity- and compatibility-validated
  artifacts, and invalidate affected dependents precisely.
- [x] **24.4 — Deterministic parallel scheduling.** Execute independent ready
  packages under a bounded job policy and prove single-job/parallel output and
  diagnostic equivalence.
- [x] Complete the Stage 24 development, sanitizer, Rust, cross-tool, native,
  cold-build, and unchanged-build exit audit.

### Stage 25: Named value enums

- [x] **25.1 — Enum contract.** Approve the implicit file kind, case syntax,
  always-public cases, file-type visibility and imports, nominal value semantics,
  initialization, equality and conversion policy, typed output and meta queries,
  representation, artifact compatibility, diagnostics, and deliberate non-goals.
  The approved contract is in [the enum proposal](docs/proposals/stage_25_enums.md).
- [x] **25.2 — Declarations and type checking.** Implement the approved syntax
  in the two-pass parser and AST; register enum types and cases before bodies;
  bind qualified cases with public access independent of spelling; enforce
  nominal assignments, overloads, initialization, type visibility, and invalid
  operations; preserve identity in typed HIR. Add parser, semantic, flow, HIR,
  and invalid-program coverage.
- [x] **25.3 — Lowering and package integration.** Implement verified enum
  constants and operations in MIR, scalar ABI storage/calls/arrays, LLVM and
  typed output, canonical identities, and bounded imported artifact records.
  Freeze the reviewed format/ABI revisions and record fixtures before changing
  serialization. Verify malformed values, type confusion, metadata ownership,
  compatibility rejection, and non-reference GC layout.
- [x] **25.4 — Equivalence and exit audit.** Test fields, constructors, static
  constants, parameters, returns, overloads, arrays and iteration, imports,
  output, and dependencies with unavailable sources. Compare direct and
  separately compiled execution, serial and parallel artifacts, and Shuttle
  reuse/invalidation after enum changes. Update owning contracts and run the
  development, sanitizer, affected cross-tool, and native suites.

### Stage 26: Value structs

- [x] **26.1 — Struct contract.** Approve implicit file identity and syntax,
  constructor visibility, explicit initialization, nominal copying, writable
  locations, final propagation, instance receiver policy, fieldwise equality,
  typed output/meta queries, and deliberate non-goals.

  - [x] Draft the source contract and identify aggregate ABI, GC, and package
    prerequisites in [the struct proposal](docs/proposals/stage_26_structs.md).
  - [x] Approve read-only value receivers and activate frontend implementation
    on 2026-09-02. Keep in-place receiver mutation deferred.

- [x] **26.2 — Frontend and value checking.** Add the struct envelope and
  symbols to the two-pass parser; enforce ordinary capitalization visibility,
  exact type identity, constructors, required field/local initialization,
  read-only/writable locations, temporary mutation diagnostics, final paths,
  permitted operations, and inline-layout cycle detection. Retain explicit
  aggregate values and storage paths in typed HIR with negative verifier tests.
  Completed on 2026-09-02 with 100/100 development and sanitizer CTests, including
  the existing cross-tool/native suites. At that checkpoint, `--check` validated
  without lowering and native/ABI/artifact support remained gated until 26.3.
  See [structs](docs/structs.md)
  and the [verification checkpoint](docs/testing.md#stage-262-struct-frontend-checkpoint).
- [x] **26.3 — Aggregate lowering and package integration.** Preserve copy,
  receiver, and result semantics through MIR, ABI, LLVM, GC, and artifacts.

  - [x] Draft exact aggregate layouts, callable passing modes, array reference-
    offset metadata, validation limits, ABI/schema revisions, and canonical
    record/layout review vectors in [the 26.3 proposal](docs/proposals/stage_26_aggregate_abi.md).
  - [x] Obtain approval and freeze that contract before implementing the
    aggregate ABI/schema boundaries. Review vectors are not `.cpa` artifacts;
    freeze full golden bytes/digests with the reviewed format-3 reader/writer.
  - [x] Implement nested inline layouts, field/index mutation with evaluate-once
    behavior, aggregate calls/results, equality, and typed output/meta queries.
  - [x] Trace embedded references in classes/arrays and root live aggregate
    locals, parameters, construction state, temporaries, and return paths.
    Protect storage owners across safepoints; reject malformed root maps.
  - [x] Preserve source-free declaration/layout/callable identity and validation;
    coordinate version requirements and opaque receipts with Shuttle without
    changing process protocol or manifest schema.

  Completed on 2026-09-02 with 121/121 development and sanitizer CTests,
  source-free native/wasm32 fixtures, full-artifact golden hashes for both target
  layouts, and all ordinary Rust checks. See the
  [implementation checkpoint](docs/testing.md#stage-263-aggregate-implementation-checkpoint).

- [x] **26.4 — Equivalence and exit audit.** Cover shallow copying, final and
  reference boundaries, nested writes with side effects, constructor flow,
  equality/NaN, arrays and iteration, overloads, class/interface calls carrying
  struct values, output, imported private layouts, and forced-GC survival.
  Verify malformed IR/ABI/artifacts, source-free dependencies, whole-project
  versus separate execution, serial/parallel bytes, and layout-change
  invalidation. Update owning documents and pass development, sanitizer,
  native/shared-tool, and Rust gates before marking the stage complete.

  - [x] Audit native value semantics, overloads, inherited/interface calls,
    constructor flow, final/reference boundaries, and malformed aggregate IR.
  - [x] Prove relocated serial/parallel struct artifacts are byte-identical;
    verify private-layout/member edits invalidate consumers while unrelated
    packages remain reusable, including source-free rejection of private access.
  - [x] Run the complete development, sanitizer, native/shared-tool, and Rust
    matrix; update both repositories' contracts and close the coordinated audit.

  Completed on 2026-09-02: 121/121 development and sanitizer CTests, including
  22 shared protocol and 12 native Shuttle cases, plus all 43 ordinary Rust
  tests and quality gates. The audit also corrected aggregate resource-limit
  diagnostic classification and bounded decoded descriptor maps. No syntax or
  compatibility revision changed. See the [exit audit](docs/testing.md#stage-264-struct-exit-audit).

### Stage 26.5.1: Explicit interface overrides

- [x] Require `override` on locally declared interface implementations and
  abstract class restatements; reject unmatched markers while preserving
  inherited implementations, final/covariant overrides, and base-only `super`.
- [x] Verify the explicit marker and replaced-class-member identity in imported
  packages without changing record shapes or physical ABI; migrate fixtures and
  test source-free, multiple-interface, inherited, and invalid contracts.
- [x] Update documentation and VS Code highlighting/snippets with regression
  tests; pass development/sanitizer, shared/native, Rust, and editor checks.

Completed on 2026-09-02: 122/122 development and sanitizer CTests, all 43
ordinary Rust tests and quality gates, and three VS Code checks with each
compiler. The covariance regression also removed source-order dependence from
class override validation. Existing ABI and artifact versions are unchanged.

### Stage 27: Switch statements and exhaustive enum handling

- [x] **27.1 — Contract.** Approve the source/flow/lowering boundaries and
  authorize implementation of the [switch proposal](docs/proposals/stage_27_switch.md).

  - [x] Draft selector and label types, grouped case blocks, exhaustiveness,
    default behavior, transfers, initialization/narrowing, MIR/GC integration,
    source-free dependencies, compatibility, resource limits, and non-goals.
  - [x] Record stage order and coordinated Shuttle work without claiming the
    feature in user documentation or modifying compiler behavior.
  - [x] Record user approval of the concrete contract and 27.2 implementation
    on 2026-09-02.

- [x] **27.2 — Frontend.** Add keyword/parser/AST support and recovery; bind and
  normalize labels; reject duplicate, incompatible, nonconstant, and missing
  cases; enforce arm scope and transfer targets. Extend typed HIR verification,
  return/required-field/final/nullable-flow analysis, and negative tests.
  Synchronize compiler/Shuttle keyword restrictions and editor highlighting.
  Gate native/IR emission explicitly until 27.3 is ready.

  Completed 2026-09-02: 127/127 development and sanitizer CTests, 43 ordinary
  Rust tests plus formatting/Clippy/Rust 1.85 checks, and six editor checks with
  each compiler. See the [frontend audit](docs/testing.md#stage-272-switch-frontend).

- [x] **27.3 — Lowering.** Add typed MIR switch terminators and full verifier,
  CFG/phi/constructor-dataflow/GC-liveness integration. Emit LLVM switches and
  invalid-enum guards; test native behavior and imported enum/static constants.
  Audit compatibility before changing any persistent format or physical ABI.

  Completed 2026-09-02: 139/139 development and sanitizer CTests, including
  forced GC, mixed phis, invalid-tag traps, maximum-size LLVM emission, and
  source-free native packages. Rust and editor gates pass. Artifact 3, compiler
  ABI 4, runtime ABI 2, and process/receipt/manifest versions are unchanged.
  See the [lowering audit](docs/testing.md#stage-273-switch-lowering).

- [x] **27.4 — Exit audit.** Cover selector side effects, scoped nested
  transfers, exhaustive returns, initialization, joins, forced GC, malformed
  representations, and case limits. Prove source-free case/constant edits,
  dependency invalidation, failed-output preservation, whole/separate behavior,
  and serial/parallel equivalence. Update `documentation/`, owning `docs/`,
  and VS Code snippets/tests; pass development/sanitizer and all shared gates.

  Completed 2026-09-02: 141/141 development and sanitizer CTests, including
  24 shared protocol and 16 native Shuttle tests; all 43 ordinary Rust tests,
  formatting/Clippy/Rust 1.85 checks, and six editor checks with each compiler.
  Enum/constant evolution, stale-consumer rejection, preserved outputs, and
  relocated serial/parallel equivalence pass. No compatibility version changed.

## Unscheduled backlog

These entries are intentionally unnumbered. They cannot be pulled into an
active stage without first updating `ROADMAP.md`.

### Language and object model

- Define immutable per-case constant data separately from per-value payloads.
  Struct-valued case data depends on approved struct and constant-initialization
  contracts; enum identity/equality must remain independent of attached data.
- Evaluate payload-bearing enums, enum members and conformance, explicit
  discriminants/underlying types, checked numeric conversion, and flags only
  under future contracts. These are not implied by Stage 25's named-value model.
- Design case enumeration and enum reflection separately from Stage 25's typed
  printing and `::typeName` contract; do not expose internal tags as a stable
  serialization format.
- Struct foundations, native lowering, and the Stage 26 exit audit are complete.
  Mutating struct receivers, struct conformance/boxing, reference returns, and
  user-defined copy/move hooks remain future contracts unless the approved
  Stage 26 scope explicitly changes.
- Implement nested type declarations. `class`, `struct`, and `enum` are
  reserved declaration starters and currently diagnosed as unsupported.
- Add flow-sensitive smart casts after a successful `value is T` test.
- Add primitive boxing before primitives may widen to `object`.
- Define the reflection surface beyond the stable `::typeName` meta query.
- Evaluate generics, traits, or an interface-based alternative after the Cloth
  1.0 boundary.
- Add first-class function values.
- Design user-defined conversions without weakening lossless implicit numeric
  conversion or checked built-in conversion.
- Decide whether Cloth synthesizes implicit default constructors.
- Decide whether distinct unit and never types belong beside `void`.
- Add reflection ability to allow "::name" meta access on declarations to return
  a `string` representation of the declaration name (if applicable).

### Expressions, numeric operations, and control flow

- Switch statements and enum exhaustiveness are complete under Stage 27 above.
  Pattern matching, destructuring, guards, ranges, and value-producing switch
  expressions remain deferred and require separate contracts.
- Add wrapping and saturating conversions as explicit primitive meta operations
  without changing the checked `NumericType(value)` contract. Their exact
  spelling, signedness behavior, and integer/floating scope remain subject to a
  future stage contract.
- Evaluate optional numeric literal suffixes as syntax ergonomics without
  changing contextual literal typing, default `int32`/`float64` inference,
  representability checks, or overload-resolution rules.
- Add floating-point bit representation and byte-order operations after the
  integer-only Stage 21 boundary is proven.
- Design recoverable exceptions, including syntax, control flow, unwinding,
  runtime representation, and a stable exception ABI.

### Optimization

- Add a general constant-folding optimizer stage after the language semantics
  it consumes are stable. Folding must preserve evaluation order, overflow and
  trap behavior, floating-point rounding, diagnostics, and target-independent
  results; it is not required for language correctness.
- Add ability to make full qualified name calls.

### Nullability

- Add nullable value types. This is required before safe access or safe meta
  queries can produce nullable primitive values.
- Add safe function calls on nullable receivers. Until then, callers narrow or
  assert the receiver first.

### Arrays, iteration, and collections

- Add contextual typing for empty and null-only array literals.
- Reify complete array element-type identity before checked `is`/`as` with
  array targets.
- Design multidimensional arrays, resizable lists, and slices without weakening
  fixed-length `T[]`.
- Add deep array equality as an explicit operation; `==` remains reference
  identity.
- Extend `for (... in ...)` beyond arrays only after defining the required
  range, binding, destructuring, async, or iterable contracts independently.

### Strings, formatting, and representation

- Add string indexing, slicing, and iteration over a deliberately selected
  Unicode unit.
- Add interpolation and type-checked formatting.
- Add case conversion, Unicode normalization, searching, and interning.
- Decide how a source-defined standard-library string API layers over the
  primitive immutable UTF-8 representation.
- Add array rendering and user-defined object-to-string formatting while
  preserving the stable default representation.

### Static storage and initialization

- Align unary-literal static initializers across frontend, retained constants,
  artifact export, and native emission. For example, `int8(-1)` in a static
  initializer is accepted by existing frontend conversion checking but is not
  a retained scalar literal. Stage 27 labels deliberately reject such fields;
  use a negative literal directly in a signed switch label. This does not
  authorize general constant folding or broaden Stage 27.

- Define deterministic static initialization order and collector root
  registration before dynamic initializers, mutable static fields, or
  reference-valued static fields.
- Define aggregate constant construction before struct-valued static constants
  or struct-backed enum metadata; Stage 26 construction does not imply compile-
  time execution or general constant folding.

### Packages, dependencies, and distribution

- Add Shuttle package registries, semantic-version selection, lockfile
  generation, and remote dependency retrieval after the local-only Stage 22
  contract.
- Define and distribute the standard library (started in std sub module).

### Backend, runtime, and tooling

- Add a WebAssembly runtime and linker path; wasm32 LLVM IR emission already
  exists.
- Expand native output beyond the current x86-64 pipeline.
- Add selectable optimization levels and debug information.
- Define command-line argument delivery to `Main`.
- Add platform packaging and distribution tooling.
- Add ability for Shuttle to build to .lib or .a (Linux/MacOS) for library files.

### Memory management

- Move compiler syntax and semantic/HIR storage to an arena or
  garbage-collected ownership model without making addresses identities.
- Reuse proven-dead shadow-stack root slots without changing the root-frame
  ABI.
- Add multi-mutator collection support before native threading.
- Evaluate finalizers, weak references, concurrent tracing, generational
  collection, and moving collection separately; each requires observable
  semantics and barrier design.
- Align the remaining VS Code new-file generators and legacy language claims
  with the current file-based language; the focused 26.5.1 override support does
  not redesign historical trait/library scaffolds.

## Intentional invariants and non-goals

These are current decisions, not unfinished work:

- Arrays are invariant; `User[]` does not widen to `object[]`.
- Cloth does not expose unchecked C-style variadic `printf`.
- Imports use identifier paths rather than string paths, and source files do
  not repeat package, module, or default class declarations.
- Object addresses, allocation identifiers, collector state, and reclamation
  timing are not source-visible behavior.
- `.` is declared member access; `::` is reserved for language-defined meta
  operations and qualified paths.
- Generics and traits are outside the Cloth 1.0 scope.
