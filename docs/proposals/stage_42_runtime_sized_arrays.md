# Proposal: Stage 42 runtime-sized fixed arrays

Status: **complete — 42.4 exit audit passed 2026-09-08**.

This proposal adds one bootstrap-critical construction form for Cloth's existing
fixed-length arrays. The array length may be computed at runtime, but the
result remains an ordinary `T[]`: it has one immutable length, checked mutable
elements, identity equality, deterministic initialization, and precise tracing.

See the [compiler roadmap](../../ROADMAP.md#stage-42-runtime-sized-fixed-arrays)
and [work ledger](../../TODO.md#stage-42-runtime-sized-fixed-arrays).

## 1. Source form

A type followed by `[:length]` constructs a fresh fixed-length array:

```cloth
int32[] offsets = int32[:source::length + 1];
Token?[] tokens = Token?[:capacity];
var flags = bool[:count];
```

The type before `[` is the element type. The expression after `:` is the
length. `T[:length]` has the exact non-null type `T[]`; it is not a list,
slice, capacity reservation, or new array type. Normal `array[index]` syntax is
unchanged, and the leading colon makes construction unambiguous from indexing.

The element type must be explicit. Context does not infer it, so `[:count]` is
invalid. An array type cannot itself be the element type at this stage;
multidimensional and jagged arrays remain deferred.

## 2. Default-initializable elements

Construction creates all elements before returning the array. It is permitted
only when Cloth defines a canonical value requiring no user code:

- `bool` defaults to `false`;
- `char` defaults to Unicode scalar U+0000;
- signed and unsigned integer primitives and aliases default to zero;
- floating-point primitives and aliases default to positive zero; and
- any otherwise valid nullable element type defaults to absent, with its
  complete representation zeroed.

Nullable class, interface, error, `object`, `string`, enum, primitive, and
struct elements are therefore supported. Existing invalid nullable types, such
as `void?`, remain invalid. Array element types remain excluded even when the
array reference is nullable because nested arrays are outside this stage.

The following non-null element types are rejected:

- `string`, file classes, interfaces, errors, and `object` because `null` is
  not a value of a non-null reference type;
- enums because Cloth does not imply a distinguished first or zero case; and
- structs because Cloth does not bypass constructors or field-initialization
  rules to invent a default value.

These rules apply even when the requested length is zero. Validity cannot
depend on a runtime count. Stage 42 does not synthesize constructors, execute a
constructor per element, or weaken non-null-by-default semantics.

## 3. Length and failure behavior

The length expression must be assignable to `int32` under the existing
lossless implicit numeric-conversion rules. An unsuffixed contextual integer
literal may become `int32`; wider, unsigned, or floating values that are not
implicitly compatible require an existing explicit checked conversion.

The length expression is evaluated exactly once before allocation. Existing
checked arithmetic and conversion failures occur before allocation. Zero is a
valid length. A negative compile-time constant is diagnosed by semantic
analysis. A negative dynamic value terminates with:

```text
cloth runtime error: array length is negative
```

Element-size multiplication and allocation-size arithmetic are checked.
Unrepresentable or impossible allocations terminate at the existing managed
allocation boundary, including the existing `array allocation is too large`
failure where applicable. Allocation failure is a deterministic terminal
runtime failure, not a typed `throws` effect. Stage 42 does not add recoverable
out-of-memory behavior.

## 4. Array behavior

The result is a fresh, non-null managed array whose length never changes.
Every element is fully initialized to the value in Section 2 before the result
becomes observable. Existing behavior is unchanged for:

- zero-based checked indexing and mutable element assignment;
- `array::length`, `for (... in array)`, and array reference identity;
- invariant array element types and independent array-reference nullability;
- left-to-right expression evaluation; and
- exact checked bounds failures.

`T[:length]` may appear anywhere an ordinary expression is allowed, subject to
the existing rules for the resulting `T[]`. It does not introduce capacity,
growth, implicit filling from another value, or structural equality.

## 5. Representation and garbage collection

The existing array header and target-derived element layout remain canonical.
The runtime allocator receives the evaluated `int32` length and the same
element-layout descriptor used by array literals. It allocates zeroed payload
storage before the array is returned.

The element descriptor retains exact size, alignment, and reference offsets:

- scalar primitives have no reference offsets;
- nullable references use their existing zero pointer representation and
  reference offset;
- nullable value structs use the Stage 41 tagged layout and shifted reference
  map; and
- absent nullable aggregate payloads are completely zero, so unconditional
  precise tracing cannot retain stale references.

Allocation remains a safepoint. Generated code must keep every live managed
value needed after the allocation in precise roots and must not publish a
partially initialized result. Stage 42 does not change ownership, collector
movement, array covariance, or the observable object model.

## 6. AST, HIR, MIR, and lowering

The AST uses a dedicated array-construction expression containing the parsed
element type and length expression. It is not represented as an array literal,
index expression, constructor call, or parser rewrite.

Typed HIR retains the resolved element type, exact result array type, checked
`int32`-compatible length, and source range. MIR normalizes the operand to
exact `int32` and uses a dedicated array-allocation
instruction with an explicit element type and length value. HIR and MIR
verifiers reject invalid element types, non-`int32` lengths, mismatched result
types, malformed ownership or references, and attempts to encode construction
as a literal with missing elements.

LLVM lowering calls the existing `cloth_rt_array_alloc(i32, layout)` entry
with a dynamic operand and the canonical target-derived descriptor. The
optimizer may simplify the length expression under existing rules, but the
allocation, its possible terminal failure, and its safepoint are observable.
They cannot be removed, duplicated, speculated, or reordered across observable
effects.

Checkpoint 42.3 lowers the dedicated MIR instruction through
`cloth_rt_array_alloc` with its dynamic `int32` operand and canonical element
descriptor. LLVM, native, interface-artifact, and object-artifact output now
support the construction. Shuttle remains syntax-opaque while carrying the
result through both targets, native linking, source-free consumption, reuse,
invalidation, deterministic publication, and failure preservation.

## 7. Artifacts, Shuttle, and the standard library

Stage 42 retains artifact/compiler/runtime compatibility
**7/6/9**, process/receipt/manifest/toolchain schemas **2/1/1/1**, and the
compiler-paired `cloth` package at v0.3.0.

Runtime-sized construction adds no exported type encoding, callable ABI,
runtime entry point, standard-library declaration, or Shuttle source policy.
Interface artifacts already describe `T[]` in fields and signatures. Object
artifacts contain the lowered implementation opaquely. Shuttle continues to
carry compiler-owned compatibility and outputs without interpreting array
syntax, lengths, defaults, or element layouts.

The existing boundary represents the approved semantics without amendment. A
future compatibility value must not change silently.

## 8. Self-host bootstrap acceptance

The first production consumer belongs to the Cloth bootstrap compiler at
`F:\Cloth`. Its Stage 42 storage slice is a specialized token buffer backed by
`Token?[]`, avoiding premature generics or a standard-library collection API:

```cloth
Token?[] values = Token?[:capacity];
```

The buffer tracks a separate logical count, writes through existing checked
indexing, and unwraps occupied elements with the existing non-null assertion.
Its capacity is computed from an in-memory source string so construction is
genuinely runtime-sized. The bootstrap smoke path must store and read an
EOF-terminated token sequence and pass direct compiler and Shuttle checks,
native execution, and deterministic rebuild coverage.

This consumer proves that Stage 42 removes a real self-hosting blocker. It does
not make the bootstrap lexer complete. Portable source-file byte input belongs
to a later stage, followed by lexer parity and then parser/AST storage.

## 9. Diagnostics

Source diagnostics cover:

- a missing element type, colon, length expression, or closing bracket;
- an unresolved, invalid, or nested-array element type;
- an element type without a canonical default value;
- a length that is not implicitly assignable to `int32`;
- a negative constant length; and
- malformed use discovered during normal parser recovery.

Diagnostics identify the rejected type or expression and use stable source
ranges. Invalid source must not reach an internal verifier diagnostic, LLVM,
artifact publication, or native execution.

## 10. Verification matrix

Implementation and exit verification cover:

- every scalar primitive and alias plus nullable primitive, enum, empty,
  nonempty, nested, and reference-bearing struct and managed-reference types;
- rejection of every non-defaultable non-null type, invalid type, and nested
  array, including zero-length requests;
- zero, one, constant, variable, computed, and maximum practical lengths in
  locals, fields, assignments, arguments, and returns;
- exact defaults, including positive floating zero and completely zero absent
  nullable payloads;
- single length evaluation, checked arithmetic/conversion ordering, negative
  constant diagnostics, negative dynamic failure, and oversized allocation;
- indexing, mutation, length queries, iteration, bounds failures, and array
  identity on constructed arrays;
- collection pressure, reference-bearing nullable structs, precise maps, and
  stale-root removal;
- malformed AST recovery plus forged HIR/MIR element, length, result, operand,
  ownership, layout, and reference state;
- verified x86-64 and wasm32 LLVM before and after optimization and native
  x86-64 execution;
- direct, whole-project, separate-package, source-free, serial, parallel, and
  relocated builds, including exact reuse, affected invalidation,
  failure-preservation, and deterministic artifacts;
- the real `F:\Cloth` token-buffer smoke path; and
- development, sanitizer, Rust/MSRV, editor, user and maintainer documentation,
  formatting, link, whitespace, and repository quality gates.

## 11. Stage plan

1. **42.1 — Contract (complete).** Freeze syntax, valid element defaults,
   length and failure behavior, representation, GC, IR, compatibility,
   diagnostics, bootstrap acceptance, verification, and non-goals.
2. **42.2 — Frontend and verified IR (complete).** Dedicated parser/AST,
   semantic analysis, HIR, MIR, diagnostics, constant-negative checking,
   malformed-state rejection, and native/artifact gates completed on
   2026-09-08.
3. **42.3 — Lowering and bootstrap integration (complete).** Both-target LLVM
   and native behavior through the existing runtime ABI, packages, source-free
   consumers, Shuttle and editor integration, user documentation, and the
   first `F:\Cloth` token-buffer consumer completed on 2026-09-08.
4. **42.4 — Exit audit (complete).** Closed defaulting, failure, evaluation,
   layout, GC, malformed-state, compatibility, determinism,
   failure-preservation, bootstrap, native/cross-target, and repository quality
   matrices on 2026-09-08.

The Stage 42.4 verification audit did not broaden this contract or change a
compatibility boundary.

## 12. Non-goals

Stage 42 does not add:

- resizable arrays, capacity APIs, lists, `Add`, `Push`, `Pop`, insertion,
  removal, or a standard-library collection type;
- generics, collection interfaces, generalized iterables, comprehensions,
  fill/repeat/factory initialization, or array-literal inference changes;
- implicit defaults for non-null managed references, enums, or structs,
  synthesized constructors, or constructor execution per element;
- multidimensional or jagged arrays, nested array elements, slices, views,
  safe indexing, safe slicing, or deep equality;
- static aggregate initialization, general constant folding, reflection,
  formatting, or serialization;
- portable file or path APIs, source-byte storage, a complete self-hosted lexer,
  parser/AST self-hosting, or compiler replacement;
- recoverable allocation errors, typed bounds failures, a moving collector,
  unsafe memory, pointers, or a public FFI; or
- an artifact, compiler ABI, runtime ABI, Shuttle schema, protocol, or
  standard-library version change.
