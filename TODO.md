# Cloth deferred-work ledger

This file is the central inventory of work deliberately left out through Stage
18. Feature documents remain authoritative for existing behavior; this
ledger records accepted gaps and their prerequisites. An unchecked item has no
implied schedule or stage number.

When completing an item, update its owning design document, implementation, and
tests in the same change, then mark it complete here with the implementing stage
or release. New entries should describe a concrete missing capability rather
than a general aspiration.

## Language and object model

- [x] Add the optional unnamed `class {}` envelope and validated
  single-inheritance graph. Completed in Stage 16.1.
- [ ] Complete the remaining file-kind declarations for an implicit file type.
  Stage 18 defines `interface {}`. `struct {}` and `enum {}` still require
  layout, value/reference semantics, construction, visibility, and import
  contracts before implementation.
- [ ] Implement nested type declarations. `class`, `struct`, and `enum` are
  reserved declaration starters today but are intentionally diagnosed as
  unsupported.
- [x] Define inherited object layout and descriptor ancestry. Stage 16.2 uses a
  complete padded base prefix, flattened GC reference maps, and a direct-parent
  descriptor link.
- [x] Define explicit base-constructor chaining. Stage 16.3 freezes
  `User(...): Human(...) { ... }`, allocates the most-derived object once, and
  runs each base's fields and constructor before derived initialization.
- [x] Decouple constructor visibility from implicit class visibility. The
  post-Stage-18 constructor refinement gives `User.co` public `User(...)` and
  private `user(...)` or `_User(...)` declarations, while enforcing access for
  ordinary and base constructor calls.
- [x] Add inherited lookup, base-reference conversions, and hierarchy-aware
  `is`/`as`. Stage 16.4 follows the base chain for visibility and runtime type
  identity.
- [x] Define explicit overrides and dynamic dispatch. Stage 16.5 requires
  `override func` for an exact inherited instance signature, preserves stable
  virtual slots, dispatches through the most-derived descriptor, and suppresses
  self dispatch during field initialization and constructor execution.
- [x] Add base-qualified calls. Stage 16.6 uses `super.Method(...)` for the
  current file class's direct-base view, retains ordinary overload lookup, and
  invokes the selected implementation directly on `self`.
- [x] Add abstract declaration syntax and identity. Stage 17.1 reserves
  `abstract` and `sealed`, accepts public bodyless `abstract func`
  declarations in an `abstract class`, retains the identity through MIR, and
  uses a verified unreachable ABI stub rather than inventing an implementation.
- [x] Reject direct abstract-class construction and require every concrete
  subclass to implement all inherited abstract slots. Stage 17.2 derives the
  transitive obligation set from the resolved virtual table while allowing
  abstract intermediates and explicit abstract-base constructor chaining.
- [x] Enforce sealed classes and final override contracts. Stage 17.3 rejects
  inheritance from a sealed file class and requires function `final` to close
  an inherited virtual slot through `final override func`.
- [x] Add covariant managed-reference override returns. Stage 17.4 accepts a
  narrower return assignable to the inherited return while keeping primitives,
  `void`, and invariant array-to-array returns exact.
- [x] Add interfaces and class conformance. Stage 18 defines interface files,
  multiple interface inheritance, `class : Human is InterfaceA, InterfaceB`,
  transitive conformance, checked conversions, deterministic dispatch tables,
  and runtime interface lookup.
- [ ] Add flow-sensitive smart casts after a successful `value is T` test.
- [ ] Add primitive boxing before primitives may widen to `object`.
- [ ] Define the intended reflection surface; Stage 15 exposes only the stable
  `::typeName` meta query.
- [ ] Add generics and traits or their eventual interface-based equivalent.
- [ ] Add first-class function values.
- [ ] Decide and implement user-defined conversions and implicit numeric
  promotions. Current overload resolution prefers exact canonical signatures
  and otherwise requires one uniquely compatible candidate.
- [ ] Add implicit default constructors if Cloth should synthesize them.
- [ ] Decide whether distinct unit and never types belong beside `void`.

## Expressions and control flow

- [ ] Implement compound assignment, increment/decrement, bitwise binary
  operators, and shifts. The lexer already recognizes the relevant tokens.
- [ ] Design recoverable exceptions, including source syntax, control-flow
  semantics, unwinding, runtime representation, and a stable exception ABI.

## Nullability

- [ ] Add nullable value types. This is a prerequisite for safe access and safe
  meta queries that produce primitives, such as a primitive field or a string
  length.
- [ ] Add safe function calls on nullable receivers. Until then, callers must
  narrow or assert the receiver first.

## Arrays, iteration, and collections

- [ ] Add contextual typing for empty and null-only array literals.
- [ ] Reify complete array element-type identity, then implement sound checked
  `is`/`as` operations for array targets.
- [ ] Design multidimensional arrays, resizable lists, and slices without
  weakening the fixed-length `T[]` contract.
- [ ] Add deep array equality as an explicit operation; `==` remains reference
  identity.
- [ ] Extend `for (... in ...)` beyond arrays with index/value bindings, numeric
  ranges, destructuring, asynchronous iteration, and a general `Iterable<T>`
  protocol.

## Strings, formatting, and representation

- [ ] Add string indexing, slicing, and iteration over a deliberately chosen
  Unicode unit.
- [ ] Add interpolation and type-checked formatting.
- [ ] Add case conversion, Unicode normalization, searching, and interning.
- [ ] Decide how a source-defined standard-library string API layers over the
  primitive immutable UTF-8 representation.
- [ ] Add array rendering and user-defined object-to-string formatting while
  preserving the stable default representation.

## Static storage and initialization

- [ ] Define deterministic static initialization order and collector root
  registration, then support dynamic initializers, mutable static fields, and
  reference-valued static fields.

## Packages, dependencies, and distribution

- [ ] Define a project manifest and dependency table.
- [ ] Add package registries, semantic version selection, and remote dependency
  retrieval.
- [ ] Define and distribute the standard library.
- [ ] Add separate compilation. File-class descriptors need coalescing or
  runtime registration so descriptor identity remains canonical across native
  modules.

## Backend, runtime, and tooling

- [ ] Add a WebAssembly runtime and linker path; wasm32 LLVM IR emission already
  exists.
- [ ] Expand native output beyond the current x86-64 pipeline.
- [ ] Add selectable optimization levels and debug information.
- [ ] Define command-line argument delivery to `Main`.
- [ ] Add platform packaging and distribution tooling.

## Memory management

- [ ] Move compiler syntax and semantic/HIR storage to an arena or
  garbage-collected ownership model without making addresses into identities.
- [ ] Reuse proven-dead shadow-stack root slots without changing the root-frame
  ABI.
- [ ] Add multi-mutator collection support before Cloth programs gain native
  threading.
- [ ] Evaluate finalizers, weak references, concurrent tracing, generational
  collection, and moving collection. Each requires its own observable-semantics
  and barrier design; none is implied by the current precise collector.

## Intentional invariants and non-goals

These are current design decisions, not unfinished checklist items:

- Arrays are invariant; `User[]` does not widen to `object[]`.
- Cloth does not expose unchecked C-style variadic `printf`.
- Imports use identifier paths rather than string paths, and source files do
  not repeat package, module, or default class declarations.
- Object addresses, allocation identifiers, collector state, and reclamation
  timing are not source-visible behavior.
- `.` is declared member access; `::` is reserved for language-defined meta
  queries and qualified paths.
