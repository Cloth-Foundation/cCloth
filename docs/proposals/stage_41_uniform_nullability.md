# Proposal: Stage 41 uniform nullability

Status: **complete — 41.4 exit audit passed 2026-09-07**.

This proposal extends Cloth's existing non-null-by-default type system from
managed references to primitive, enum, and struct values. It preserves the
Stage 12 presence, narrowing, coalescing, assertion, and safe-access model while
giving value nullability an explicit, deterministic representation.

See the [compiler roadmap](../../ROADMAP.md#stage-41-uniform-nullability) and
[work ledger](../../TODO.md#stage-41-uniform-nullability).

## 1. Type surface

A trailing `?` admits the `null` value for any supported concrete type:

```cloth
int32? count = null;
bool? enabled = false;
Status? status = Status.Ready;
Point? point = Point(2, 4);
```

Primitive aliases retain their canonical identity: `int?`, `uint?`, and
`float?` are `int32?`, `uint32?`, and `float32?`. Nullable value types are
permitted in locals, parameters, returns, instance fields, and array elements.
Array and element nullability remain independent:

```cloth
int32?[] values;       // non-null array of nullable integers
int32[]? values;       // nullable array of non-null integers
int32?[]? values;      // nullable array of nullable integers
```

Managed references retain their existing pointer-nullable representation.
`string?`, class references, interface references, `object?`, error references,
and nullable array references do not become tagged value wrappers.

`void?` is invalid because `void` has no value. `null?`, internal error or
bottom types, and repeated nullable qualification such as `int32??` are also
invalid. Safe operations flatten nullable results rather than creating nested
nullable types.

Stage 41 does not make structs or primitives managed references. A nullable
value is inline data, not a heap object, and it does not acquire object members,
reference casts, inheritance, virtual dispatch, or boxing.

## 2. Assignment, conversion, and inference

For an underlying type `T`:

- `T` is assignable to `T?` by creating a present value;
- `null` is assignable to `T?` by creating an absent value;
- `T?` is not assignable to `T` without narrowing, `!`, or `??`; and
- `T?` is assignable to `U?` exactly when the existing implicit conversion from
  `T` to `U` is valid. Absence remains absence and a present payload is
  converted once.

Lifted conversions include lossless numeric widening, derived-to-base and
class-to-interface reference widening, and the existing nullable reference
conversions. They do not add numeric narrowing, enum conversion, struct
conversion, user-defined conversion, or value-to-`object` boxing.

An expected `T?` type first contextualizes a literal as `T`, then wraps the
checked value. This applies uniformly to initializers, assignments, arguments,
returns, array elements, conditional joins, and coalescing fallbacks.

`var value = null;` remains invalid because `null` supplies no underlying type.
`var value = expression;` retains `T?` when the expression has that type.
Array literals infer the common non-null element type before applying nullable
qualification. Examples include `[1, null]` as `int32?[]` and `[point, null]`
as `Point?[]`. Empty and null-only array literals still require a future
contextual-array-literal contract.

Overloads continue not to differ only by nullable qualification. This rule is
uniform across reference and value types and avoids a source-level distinction
that depends on physical representation. Parameter matching otherwise uses the
existing exact-then-unique-conversion order. Overrides and interface
implementations require the existing parameter compatibility and return rules;
nullable value returns have exact nominal or primitive identity because value
types have no covariance.

## 3. Presence, narrowing, and equality

Every `T?` value supports the existing presence model:

```cloth
int32? count = ReadCount();
if (count) {
  println(count);
}
```

A nullable condition tests presence, not its payload. This rule includes
`bool?`: a present `false` value satisfies the outer presence condition. Test
the payload only after narrowing or assertion:

```cloth
bool? enabled = false;
if (enabled) {                 // present
  bool isEnabled = enabled;    // narrowed payload
  if (isEnabled) {
    println("enabled");
  }
}
```

Prefix `!` on a nullable value tests absence. Direct `== null` and `!= null`
comparisons, reversed null operands, parentheses, logical negation, `&&`, and
`||` retain the current flow rules. Stable locals and parameters narrow to `T`
on proven paths. Assignment invalidates the proof, branch joins retain only
facts established on every fallthrough path, and fields remain unnarrowed
because aliases and calls can change them.

`T?` supports `==` and `!=` with `null`, a compatible `T`, or a compatible
`T?` whenever the underlying type supports equality. Two absent values are
equal; exactly one absent value is unequal; two present values use the existing
underlying equality. This preserves floating-point NaN behavior, enum identity,
fieldwise struct equality, string contents, and managed-reference identity.
Ordering, arithmetic, bitwise operations, indexing, iteration, and `switch`
selection require a proven or extracted non-null value.

## 4. Coalescing and assertion

The existing operators apply uniformly:

```cloth
int32 count = optionalCount ?? 0;
Point point = optionalPoint ?? Point(0, 0);
int32 required = optionalCount!;
```

`left ?? fallback` evaluates `left` once and evaluates the fallback only when
`left` is absent. For a `T?` left operand, a fallback assignable to `T` produces
`T`; a fallback assignable only to `T?` produces `T?`. Lifted implicit
conversions occur once on the selected path. `?? throw Error()` retains its
current typed-error behavior.

Postfix `value!` evaluates its operand once. A present value produces one copy
of its `T` payload. An absent value terminates with the existing exact runtime
failure:

```text
cloth runtime error: non-null assertion failed
```

The assertion is not a typed `throws` effect. A successful assertion of a
stable local or parameter establishes the normal flow fact for later reads.

## 5. Safe fields and function calls

`receiver?.Field` continues to evaluate its nullable receiver once. If the
receiver is absent, it returns null without loading the field. If present, it
reads the field once and wraps a non-null field result. Reference, primitive,
enum, and struct fields are supported; an already-nullable field remains a
single nullable layer.

Stage 41 also enables safe instance-function calls:

```cloth
int32? age = user?.GetAge();
Point? moved = point?.Moved(1, 2);
logger?.Flush();
```

The receiver is evaluated first. When it is absent, no argument is evaluated,
no function body executes, and no call-originated error is produced. When it is
present, arguments evaluate left-to-right exactly once and ordinary virtual,
interface, error, or read-only struct dispatch applies.

A non-`void` result `R` becomes `R?`, flattened when `R` is already nullable. A
`void` call remains `void` and becomes a no-op on an absent receiver; Cloth does
not create `void?`. The callable's declared `throws` set remains part of the
containing function's static effect because the call may execute. Static
functions, constructors, fields used as callables, `super`, and non-null
receivers are invalid safe-call targets.

Safe calls preserve source visibility, overload selection, override dispatch,
receiver snapshots, argument ownership, and precise-root requirements. This
checkpoint adds no first-class callable value.

## 6. Safe meta queries

Safe meta access uses `?::`, preserving the distinction between declared
members and compiler-defined metadata:

```cloth
int32? length = maybeText?::length;
bool? empty = maybeText?::isEmpty;
string? name = maybeObject?::typeName;
```

`receiver?::query` requires a nullable value receiver and a non-callable meta
query supported by its underlying type. It evaluates the receiver once. An
absent receiver produces null without executing the query; a present receiver
executes the query once. A result `R` becomes `R?`, flattening an existing
nullable result.

The parser treats `?::` as one postfix operator even if the lexer retains `?`
and `::` as separate tokens. Names remain case-sensitive and use the existing
lower-camel meta namespace. `?.` remains declared member access and `?::`
remains language-defined meta access.

Safe callable meta operations are not included. In particular,
`text?::slice(start, end)` is diagnosed rather than silently adding safe
slicing or conditional bound evaluation. Narrow or assert the receiver before
calling a meta operation. Safe indexing likewise receives no new syntax.

## 7. Tagged value representation

A nullable value type has a canonical inline representation containing an
8-bit presence tag followed by an aligned `T` payload:

```text
tag offset     = 0
payload offset = align_up(1, alignof(T))
alignment      = alignof(T)
size           = align_up(payload offset + sizeof(T), alignment)
```

The tag is exactly `0` for absent and `1` for present. Other tag values are
invalid generated-program or runtime state and cannot be source-constructed.
An absent value has an all-zero payload; this is required for deterministic
initialization and precise tracing, not source-visible bit access. Padding and
allocation identity remain unobservable.

A present value contains a valid ordinary `T` payload. Copies copy the logical
tag and payload without invoking user code. Setting a value to null clears its
payload so stale references cannot remain roots. Generated code may not expose
a safepoint while a tagged value is in an inconsistent transitional state.

Nullable structs shift the underlying struct's reference-offset map by the
payload offset. The collector may scan those offsets unconditionally because
an absent payload is zero. Nullable primitives and enums contain no reference
offsets. Nullable references remain a single pointer with reference offset
zero and do not use this wrapper.

Non-final nullable locals and mutable class fields without initializers default
to absent, matching existing nullable-reference initialization. Final locals
still require initializers, and final class fields require one declaration or
constructor initialization. Struct constructors retain their existing rule
that every field must be initialized on every exit, including nullable fields.
Parameters are always supplied by callers. Existing static-field and
required-constant restrictions remain in force; Stage 41 does not introduce
aggregate or general nullable constants.

Layout arithmetic is checked before allocation or emission. The underlying
payload retains its existing size/depth/reference-map limits; the wrapper may
add only its computed tag, alignment padding, and tail padding. Callable
aggregate backing storage retains the existing 256 KiB cap.

## 8. ABI, IR, and runtime

HIR retains nullable value types on expressions and uses the existing explicit
safe-access, coalescing, assertion, and flow representations, extended for safe
calls and safe meta queries. MIR represents present construction, absent
construction, presence testing, payload extraction, and lifted conversion
explicitly. Verifiers distinguish pointer-nullable references from tagged
nullable values and reject representation-dependent shortcuts before LLVM
lowering.

Nullable value parameters and returns use the existing aggregate ABI policy:
explicit arguments receive independent value-pointer copies and results use
caller-owned result storage. Throwing functions keep their nullable managed
`Error` return and write a successful nullable value through the ordinary result
storage. No nullable value is heap allocated solely for parameter passing,
returning, safe access, or assertions.

LLVM uses target-derived wrapper sizes and alignments for x86-64 and wasm32.
Branches inspect the tag; payload loads occur only on present paths. Phi/control
flow, storage copies, and GC-root publication must preserve complete values and
observable evaluation order before and after optimization.

Runtime ABI 9 adds one narrow checked-presence entry for tagged assertions:

```cpp
extern "C" void cloth_rt_require_nullable_value(std::uint8_t tag) noexcept;
```

Tag `1` returns, tag `0` terminates with `non-null assertion failed`, and every
other tag terminates with `nullable value has an invalid presence tag`. The
runtime owns those exact failure reasons but does not interpret payload bytes,
perform nullable conversions, box values, or own safe-call dispatch. Existing
reference assertions continue through their pointer check.

## 9. Artifacts, Shuttle, and the standard library

Stage 41.3 advances artifact/compiler/runtime compatibility from **6/5/8** to
**7/6/9**. Artifact format 7 broadens the existing nullable type record to
primitive, enum, and struct elements and records the tagged aggregate layout
and shifted reference map. Readers reconstruct the tag and payload offsets from
the target layout and reject noncanonical sizes, alignments, maps, identities,
and callable modes.

Compiler ABI 6 distinguishes nullable value encodings and uses `_C6` native
names. The source rule still prohibits overloads that differ only by
nullability. Existing nullable references retain pointer layout; every package
is nevertheless rebuilt because compiler ABI identity and artifact format move
together. Formats 1–6 and compiler/runtime ABI mismatches are rejected and
rebuilt, never migrated or reinterpreted.

Shuttle remains opaque to source nullability and payload representation. It
carries format 7, compiler ABI 6, and runtime ABI 9 through existing capability,
receipt, compiler-identity, cache, invalidation, and link fields. Process
protocol 2 and receipt/manifest/toolchain-metadata schemas 1/1/1 remain
unchanged.

The compiler-paired `cloth` standard library remains v0.3.0 and adds no public
declaration during Stage 41. Its source and artifacts rebuild under 7/6/9 for
both targets. Exact selection, source-free consumption, reuse, invalidation,
and native linking remain coordinated compiler/Shuttle responsibilities.

User documentation is updated only when 41.3 makes the complete feature
executable. Checkpoint 41.2 may expose frontend validation through `--check`,
but LLVM, native, and artifact publication must reject nullable value types
until layout, ABI, runtime, and package support arrive together.

## 10. Diagnostics and verification

Source diagnostics cover unsupported underlying types, repeated `?`, missing
context for `null`, invalid assignment/conversion, nullable use without proof,
non-null safe receivers, safe static/constructor calls, invalid safe meta names,
callable safe meta operations, incompatible equality/coalescing, and forbidden
overload or override distinctions.

Implementation verification covers:

- every primitive and alias, enum, empty/nonempty/nested struct, and struct with
  managed references;
- absent and present locals, parameters, returns, fields, array elements,
  assignments, conditional joins, and lifted conversions;
- `bool?` presence versus payload truth, direct/reversed null comparisons,
  nested narrowing, mutation invalidation, guard clauses, loops, and field
  non-narrowing;
- equality for absent/present pairs and every supported underlying equality;
- exact lazy coalescing, assertion success/failure, and receiver/argument order;
- safe field and safe function behavior for classes, interfaces, errors, and
  structs, including `void`, nullable, throwing, and virtual results;
- `?::length`, `?::byteLength`, `?::isEmpty`, and `?::typeName`, plus rejection
  of safe callable meta operations and safe indexing;
- canonical tag/payload layouts, default absence, copies, aggregate passing,
  shifted reference maps, collection pressure, and stale-root removal;
- malformed HIR, MIR, ABI, artifact type/layout records, sizes, alignments,
  maps, operands, results, callable modes, and compatibility values;
- format-7/compiler-ABI-6/runtime-ABI-9 rebuild, link, exact reuse,
  affected-invalidation, failure-preservation, and relocated determinism;
- verified x86-64 and wasm32 LLVM before and after optimization, native x86-64
  execution, and direct/whole/separate/source-free/Shuttle equivalence; and
- development, sanitizer, Rust/MSRV, editor, user and maintainer documentation,
  formatting, links, whitespace, and repository quality gates.

## 11. Stage plan

1. **41.1 — Contract (complete).** Freeze supported types, conversions,
   inference, presence, equality, operators, safe access/calls/meta queries,
   tagged layout, GC, compatibility, diagnostics, verification, and non-goals.
2. **41.2 — Frontend and verified IR (complete).** Implement parsing, typing, flow,
   equality, safe operations, dedicated HIR/MIR behavior, diagnostics, and
   malformed-state rejection while retaining the native/artifact gate.
3. **41.3 — Lowering and integration (complete).** Implement tagged ABI/storage, runtime
   ABI 9, LLVM/native lowering, GC maps, artifact format 7, compiler ABI 6, both
   targets, packages, source-free consumers, Shuttle, editor support, and user
   documentation.
4. **41.4 — Exit audit (complete).** Close type, flow, evaluation, layout, GC,
   malformed state, compatibility, determinism, failure-preservation,
   native/cross-target, and repository quality matrices.

The 41.4 audit closes every verification item in Section 10, including
deliberate repeated-nullability diagnostics, all-value equality and flow,
cross-receiver safe dispatch, malformed IR/ABI/artifact records, shifted GC
maps and stale-root removal, exact end-to-end evaluation order, package failure
preservation, both LLVM targets, native execution, and all repository gates.
Compatibility remains artifact/compiler/runtime **7/6/9**, schemas remain
**2/1/1/1**, and `cloth` remains v0.3.0.

## 12. Non-goals

Stage 41 does not add:

- `Option`, `Maybe`, algebraic-data-type syntax, pattern matching,
  destructuring, or nullable type parameters;
- repeated/nested nullable wrappers, implicit unwrapping, implicit truthiness
  of non-null values, or a distinct `undefined` value;
- primitive or struct boxing, value-to-`object` widening, checked value casts,
  inheritance, interfaces for structs, or user-defined conversions;
- safe indexing, safe slicing, safe callable meta operations, ranges, views,
  string searching, or collection APIs;
- direct printing or formatting of absent values, interpolation, user-defined
  null behavior, operator overloading, or reflection additions;
- nullable required constants, aggregate constants, dynamic static
  initialization, default struct values, or synthesized constructors;
- local error recovery, conversion of null failures into typed errors, or a
  change to declared `throws` behavior;
- new targets, public FFI, general ABI optimization, a moving collector, or a
  WebAssembly runtime; or
- enum payloads/metadata, nested types, generics, traits, first-class functions,
  collections, or unrelated language and toolchain work.
