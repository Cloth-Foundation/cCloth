# Proposal: Stage 40 Unicode string slicing

Status: **complete — 40.4 exit audit passed 2026-09-07**.

This proposal adds checked slicing to Cloth's immutable UTF-8 `string` after
Stage 39 established Unicode scalar literals, indexing, and linear traversal.
It defines one intrinsic value operation without introducing ranges, views,
general iterators, or a source-defined string-method system.

See the [compiler roadmap](../../ROADMAP.md#stage-40-unicode-string-slicing)
and [work ledger](../../TODO.md#stage-40-unicode-string-slicing).

## 1. Source surface

A non-null string value supports one lowercase intrinsic operation:

```cloth
string text = "A🧵BC";
string middle = text::slice(1, 3); // "🧵B"
```

Its grammar uses existing postfix meta-access and call syntax:

```text
string_slice = expression "::" "slice" "(" expression "," expression ")" ;
```

`slice` remains an identifier rather than a reserved keyword. The operation is
available only through a string value. It is not a declared member, cannot be
accessed with `.`, and cannot be referenced as a first-class callable. Exactly
two arguments are required; Stage 40 adds no omitted-bound or overload forms.

Both arguments must be assignable to `int32`, matching string and array index
typing. Contextual integer literals therefore work normally. The exact result
type is non-null `string`.

## 2. Scalar bounds and result

`text::slice(start, end)` selects the half-open Unicode scalar interval
`[start, end)`. It never interprets either argument as a UTF-8 byte offset. A
slice is valid exactly when:

```text
0 <= start <= end <= text::length
```

Equal bounds produce an empty string, including `(0, 0)` on an empty input and
`(text::length, text::length)` on any input. `(0, text::length)` produces the
same string contents as the receiver. Allocation identity, backing-storage
reuse, and empty-string canonicalization are not observable Cloth behavior.

The result contains the exact canonical UTF-8 byte subsequence between the two
scalar boundaries. Embedded U+0000 is ordinary string data and does not end the
slice. Combining marks remain independent scalars. Slicing performs no
normalization, grapheme segmentation, case conversion, or locale processing.

The result is immutable and has the existing content-based string equality,
scalar length, byte length, printing, parsing, indexing, and iteration
behavior. It is not a writable location or a view into mutable storage.

## 3. Evaluation and failure behavior

The receiver is evaluated first, followed by `start` and then `end`, exactly
once each. Ordinary bottom-value and typed-error propagation applies: a
terminal receiver or argument prevents evaluation of every later operand. All
three successful evaluations complete before runtime bounds validation, just
like arguments to an ordinary call.

The receiver remains reachable throughout argument evaluation, bounds
validation, byte-boundary discovery, and result allocation. Temporary managed
values produced by the receiver or either argument follow normal precise-root
liveness.

Invalid bounds terminate with this exact checked-runtime message:

```text
cloth runtime error: string slice is out of bounds
```

Negative bounds, `start > end`, and `end > text::length` use the same failure.
It is not a typed `throws` effect, and no expression or statement after the
terminal operation executes. A nullable receiver must first be narrowed or
asserted non-null; Stage 40 adds no safe slicing.

## 4. Complexity and allocation

The runtime discovers both scalar boundaries with one monotonically advancing
UTF-8 cursor. A slice is O(`text::byteLength`) in the worst case and uses
constant auxiliary space excluding returned string storage. It must not build
an offset table or repeatedly index from scalar zero.

The logical operation may allocate one managed string. An implementation may
reuse the input for a complete slice or an existing empty representation
because allocation identity is unobservable. Such reuse cannot change content,
rooting, failure, or complexity behavior.

## 5. Compiler representation and verification

The parser retains the operation through the existing meta-call syntax.
Semantic analysis recognizes `slice` only for a non-null string value, checks
arity and argument assignment compatibility, and records the exact string
result.

HIR uses a dedicated string-slice expression containing the receiver, start,
and end expressions. MIR uses a dedicated string-slice instruction after both
bounds are explicitly coerced to `int32`. Neither representation models the
operation as a user call, array slice, repeated scalar index, or source-level
loop.

HIR verification requires the exact non-null string receiver, `int32`-compatible
bound operands, non-null string result, value category, operand order, and
reachable references. MIR verification requires exact `int32` bounds after
coercion and preserves the remaining invariants. Verification runs before ABI
lowering and again after MIR optimization. Optimization may eliminate an unused
slice only when it proves that receiver and bound evaluation plus every possible
terminal failure remain observable in the required order. It may not speculate
the allocation across control flow.

Checkpoint 40.2 implements and verifies these internal representations. The
semantic marker distinguishes the callable meta operation from ordinary calls;
dedicated HIR retains `int32`-compatible source bounds, and dedicated MIR owns
their explicit `int32` coercions. Bottom-valued bounds preserve terminal flow
without fabricating an executable slice. The optimizer remaps all three MIR
operands but does not classify slicing as foldable.

Checkpoint 40.3 lowers the dedicated MIR instruction to runtime ABI 8 for both
supported LLVM targets. It retains the receiver as a precise GC root across
the allocating call and publishes the returned managed string only after the
call completes. Native, package, source-free, and Shuttle execution now expose
the operation as one supported feature.

## 6. Runtime and GC ownership

Runtime ABI 8 adds one complete operation:

```cpp
extern "C" void* cloth_rt_string_slice(
    const void* value, std::int32_t start, std::int32_t end) noexcept;
```

The runtime validates the non-null managed-string layout, byte length, scalar
count, canonical UTF-8 encoding, scalar bounds, and decoded boundaries. It
returns a managed immutable string with correct byte and scalar metadata.
Malformed layout or decoding state is an internal runtime failure rather than
a source-visible bounds error or replacement character.

Unlike Stage 39 indexing and cursor advancement, slicing may allocate and is a
safepoint. The compiler roots the receiver across the call, and the runtime
keeps any source storage valid until copying or safe reuse is complete. The
operation exposes no address, byte pointer, capacity, allocation identity, or
collector state.

## 7. Packages, artifacts, and Shuttle

Stage 40 changes no serialized declaration or constant representation.
Artifact format **6** and compiler ABI **5** remain unchanged. Checkpoint 40.3
advanced runtime ABI **7 to 8** with the runtime operation and every lowering
path available together.

Process protocol **2**, receipt schema **1**, manifest schema **1**, and
toolchain-metadata schema **1** remain unchanged. The compiler-paired `cloth`
standard library adds no public declaration and remains v0.3.0. Its unchanged
source distribution is rebuilt and selected under runtime ABI 8.

Direct, whole-project, separate-package, and source-free consumers must agree
on slicing behavior. Shuttle treats the source operation and string contents
as opaque. It carries runtime ABI 8 through existing capability, receipt,
compiler-identity, cache, invalidation, link, and atomic-publication fields.
Older runtime-ABI artifacts are rebuilt rather than linked into an ABI-8
program.

Current compatibility is artifact/compiler/runtime **6/5/8**, schemas
**2/1/1/1**, and `cloth` v0.3.0.

## 8. Diagnostics and verification

Source diagnostics must cover a nullable or non-string receiver, type-level
use, `.` member spelling, unknown or case-mismatched meta names, missing or
extra arguments, non-`int32`-compatible bounds, use as a first-class value, and
assignment through the result. Bounds failures retain the exact runtime
message and do not create duplicate compiler diagnostics.

Implementation verification must cover:

- empty, ASCII, embedded-U+0000, combining, BMP, non-BMP, mixed-width, and
  U+10FFFF strings;
- empty-at-zero, empty-at-middle, empty-at-length, prefix, suffix, interior,
  one-scalar, and complete slices;
- negative start, negative end, reversed bounds, end at length plus one, and
  large invalid bounds;
- exact receiver/start/end order and once-evaluation, terminal operand
  propagation, bounds timing, and absence of later side effects after failure;
- receiver rooting during allocating bound expressions and result allocation,
  explicit collection pressure, empty/full reuse freedom, and long strings;
- one-pass monotonic boundary discovery and rejection of malformed HIR, MIR,
  managed-string layouts, scalar counts, UTF-8, and decoded boundaries;
- unchanged string literals, equality, concatenation, meta queries, indexing,
  iteration, printing, parsing, input, nullability, and optimizer behavior;
- unchanged array indexing and iteration without admitting array slicing;
- format-6/compiler-ABI-5/runtime-ABI-8 capability, rebuild, link, reuse,
  affected-invalidation, failure-preservation, and relocated determinism
  matrices;
- verified x86-64 and wasm32 LLVM before and after optimization, native x86-64
  execution, and direct/whole/separate/source-free/Shuttle equivalence; and
- development, sanitizer, Rust/MSRV, editor, user and maintainer documentation,
  formatting, links, whitespace, and repository quality gates.

## 9. Stage plan

1. **40.1 — Contract (complete).** Freeze the intrinsic syntax, Unicode-scalar
   bounds, immutable result, evaluation and failure behavior, complexity,
   allocation and GC ownership, compatibility transition, diagnostics,
   verification, and non-goals.
2. **40.2 — Semantic and verified IR (complete).** Implement string-slice
   binding, typing, dedicated HIR/MIR, malformed-state rejection, control-flow
   effects, and focused compiler tests without declaring a releasable partial
   feature.
3. **40.3 — Runtime and toolchain integration (complete).** Implement runtime
   ABI 8, allocation and rooting, LLVM and native execution, both targets,
   packages, source-free consumers, Shuttle coordination, editor support, and
   user documentation.
4. **40.4 — Exit audit (complete).** Close Unicode, bounds, evaluation,
   allocation, complexity, GC, malformed-state, compatibility, determinism,
   failure-preservation, native/cross-target, and repository quality matrices.

All four checkpoints are complete. The 40.4 audit confirms the approved
contract across development and sanitizer builds, both targets, native and
source-free execution, malformed state, Shuttle determinism, editor coverage,
documentation, and repository gates. String slicing is now a complete,
releasable Stage 40 feature; no later stage is activated by this closure.

## 10. Non-goals

Stage 40 does not add:

- omitted, inclusive, negative-from-end, clamped, wrapping, or saturating
  bounds;
- bracket slicing, range syntax or values, strides, reverse slicing, or
  destructuring;
- array, list, byte-array, object, or user-defined slicing and overloading;
- borrowed views, mutable views, substring mutation, exposed UTF-8 storage, or
  stable allocation identity;
- grapheme-cluster, glyph, byte, UTF-16-unit, normalized, case-folded, or
  locale-aware slicing;
- safe slicing, nullable value types, typed bounds errors, local recovery, or
  migration of existing index failures into typed errors;
- searching, containment, splitting, replacement, trimming, interpolation,
  formatting, normalization, or case conversion;
- general constant folding, compile-time slicing, static execution, interning,
  or a source-defined standard-library string API;
- new artifact/compiler/protocol/schema/library versions, targets, public FFI,
  or a WebAssembly runtime; or
- enum metadata, aggregate constants, static initialization, collections,
  generics, traits, or unrelated language and toolchain work.
