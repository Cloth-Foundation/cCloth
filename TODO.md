# Cloth work ledger

`ROADMAP.md` defines stage order and scope. This file tracks the concrete work
inside scheduled stages and preserves accepted but unscheduled gaps. Implemented
behavior belongs in `docs/` rather than in a growing history of checked boxes.

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
Attached constant data and runtime payloads remain deferred. No later stage is active.

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
- Define structs as an implicit file kind. A future stage must freeze value
  semantics, layout, copying, construction, methods, conformance and inheritance
  rules, managed-reference fields, equality, visibility, imports, and ABI
  behavior before implementation.
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

### Expressions, numeric operations, and control flow

- Design `switch` or pattern matching, including exhaustiveness and evolution
  of closed case sets, before adding enum-specific control-flow syntax.
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

- Define deterministic static initialization order and collector root
  registration before dynamic initializers, mutable static fields, or
  reference-valued static fields.

### Packages, dependencies, and distribution

- Add Shuttle package registries, semantic-version selection, lockfile
  generation, and remote dependency retrieval after the local-only Stage 22
  contract.
- Define and distribute the standard library.

### Backend, runtime, and tooling

- Add a WebAssembly runtime and linker path; wasm32 LLVM IR emission already
  exists.
- Expand native output beyond the current x86-64 pipeline.
- Add selectable optimization levels and debug information.
- Define command-line argument delivery to `Main`.
- Add platform packaging and distribution tooling.

### Memory management

- Move compiler syntax and semantic/HIR storage to an arena or
  garbage-collected ownership model without making addresses identities.
- Reuse proven-dead shadow-stack root slots without changing the root-frame
  ABI.
- Add multi-mutator collection support before native threading.
- Evaluate finalizers, weak references, concurrent tracing, generational
  collection, and moving collection separately; each requires observable
  semantics and barrier design.

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
