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

## Active stage

No feature stage is active. Stage 20 is complete. The next scheduled action is
the Stage 21.1 design contract; implementation must not begin until that
contract and its explicit implementation start are approved.

## Scheduled work

### Stage 21: Integer binary representation and byte order

- [ ] **21.1 — Integer operator contract.** Freeze valid operand types, result
  types, precedence, signed complement, signed right shift, shift-count bounds,
  compound assignment, constant behavior, and diagnostics for `&`, `|`, `^`,
  `~`, `<<`, and `>>`.
- [ ] **21.2 — Integer operator implementation.** Carry the approved contract
  through parser/AST, semantic analysis, HIR, MIR verification, LLVM lowering,
  invalid-program coverage, and native execution tests.
- [ ] **21.3 — Byte-order contract.** Approve the source surface and semantics
  for explicit little-endian and big-endian encoding and decoding of fixed-width
  integers. Specify byte-array size, offset, bounds, signedness, evaluation
  order, and failure behavior without exposing host-native memory.
- [ ] **21.4 — Byte-order implementation.** Implement the approved surface,
  retain the operation explicitly through verified compiler representations,
  and test identical byte sequences independent of target layout metadata.
- [ ] Complete the Stage 21 exit audit in `ROADMAP.md`, including development
  and sanitizer suites and recording every deliberate deferral below.

### Stage 22: Project manifest and local dependencies

- [ ] **22.1 — Manifest contract.** Define a versioned `cloth.toml` schema for
  package identity, source roots, entry files, and local path dependencies.
- [ ] **22.2 — Dependency graph.** Resolve local dependencies deterministically
  and diagnose cycles, duplicate identities, missing paths, and invalid source
  roots before source parsing.
- [ ] **22.3 — CLI integration.** Make project discovery and compilation consume
  the manifest while preserving identifier-based imports.
- [ ] **22.4 — Project verification.** Add parser/configuration unit tests,
  multi-project integration fixtures, build documentation, and the Stage 22
  development/sanitizer exit audit.

### Stage 23: Separate compilation and deterministic linking

- [ ] **23.1 — Artifact contract.** Define the versioned package artifact and
  the semantic, MIR/ABI, and dependency metadata it owns.
- [ ] **23.2 — Canonical identity.** Preserve type descriptors, interface
  identities, mangled callables, and runtime registration across artifacts.
- [ ] **23.3 — Link pipeline.** Link local package artifacts deterministically
  and diagnose duplicate, missing, or incompatible definitions.
- [ ] **23.4 — Equivalence verification.** Compare separate and whole-project
  compilation in ABI, linker, invalid-input, and native execution tests, then
  complete the Stage 23 development/sanitizer exit audit.

## Unscheduled backlog

These entries are intentionally unnumbered. They cannot be pulled into an
active stage without first updating `ROADMAP.md`.

### Language and object model

- Define enums as an implicit file kind. A future stage must freeze case syntax,
  discriminants and payload policy, representation, construction, equality,
  visibility, imports, and control-flow integration before implementation.
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

- Add package registries, semantic-version selection, lockfiles, and remote
  dependency retrieval after the local-only Stage 22 contract.
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
