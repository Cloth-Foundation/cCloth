# Proposal: Stage 26 value structs

Status: **approved and implemented; coordinated 26.4 exit audit complete**.

The source contract and implementation start were approved on 2026-09-02,
including read-only value receivers. Aggregate ABI/schema decisions were
approved by the separate 26.3 review on 2026-09-02 and are now implemented.
The [roadmap](../../ROADMAP.md#stage-26-value-structs)
and [work ledger](../../TODO.md#stage-26-value-structs) own scope and sequencing.

The [struct implementation](../structs.md) supports frontend checking, native
execution, and source-free packages. The
[26.3 aggregate ABI contract](stage_26_aggregate_abi.md) freezes the exact layout,
callable, runtime, artifact, and resource-limit decisions without changing the
approved source semantics.

The [26.4 exit audit](../testing.md#stage-264-struct-exit-audit) passed on
2026-09-02 without expanding this source contract or its non-goals.

## Purpose and scope

A struct groups fields into a nominal value. Assignment, parameters, returns,
and array reads copy that value; they do not introduce shared object identity.
Reference-valued fields still refer to managed objects. Structs complement
classes and enums without adding manual memory management.

The stage includes fields, constructors, static functions, read-only instance
functions, nested struct *values*, arrays, fieldwise equality, typed output,
GC integration, and separate compilation. Nested type *declarations*, receiver-
mutating methods, inheritance, interface conformance, boxing, nullable values,
aggregate static constants, and enum-attached data are outside this proposal.

Read-only instance functions are the approved receiver policy. In-place receiver
mutation remains deferred; it would require a separate language-contract review.

## File identity and declarations

```cloth
// Point.co
struct {
  int32 X;
  int32 Y;

  Point(int32 x, int32 y) {
    X = x;
    Y = y;
  }

  func Moved(int32 dx, int32 dy): Point {
    return Point(X + dx, Y + dy);
  }
}
```

The filename supplies `Point`; `struct Point { ... }` is invalid. Imports may
precede the envelope. Other declarations cannot follow it. An unwrapped file
still defines a class. The existing reserved `struct` token is used; no keyword
is added.

Approved grammar addition:

```ebnf
explicit_file_type = explicit_file_class | explicit_interface
                  | explicit_enum | explicit_struct ;
explicit_struct   = "struct" "{" { member_declaration } "}" ;
```

Contextual checks limit the existing member grammar to supported struct members.
`abstract`, `sealed`, inheritance/conformance clauses, `override`, `final func`,
bodyless functions, constructor base initializers, and `super` are invalid.
Struct instance calls are direct, never virtual.

File, field, function, and constructor visibility follow the existing ASCII
capitalization rules. Unlike enum cases, struct fields are not automatically
public. `Point(...)`, `point(...)`, and `_Point(...)` retain the existing public
and private constructor spellings; calls always use `Point(...)`.

Identity includes the canonical package/version, source namespace, file stem,
and a distinct `struct` nominal kind. Equal fields do not make two structs
compatible. Type/member registration precedes body checking. Existing aliases,
wildcard type imports, private access, and ambiguity diagnostics remain intact.

Static fields declared in a struct retain the supported scalar-literal or enum-
case `static final` contract. A struct-valued static field is not introduced.
Static `Main` eligibility remains based on its public, parameterless,
`void`/`int32` signature; a struct envelope does not require an instance entry.

## Copying and value boundaries

```cloth
Point first = Point(1, 2);
Point second = first;
second.X = 10;
println(first.X);                 // 1

Point[] points = [first, second];
Point selected = points[0];
selected.Y = 20;                  // Does not change points[0].
points[0].Y = 30;                 // Explicitly changes the stored element.
```

Copying recursively copies embedded struct fields and copies primitive/enum
values. A class, interface, string, array, or nullable reference field copies
its reference; the referenced object is not cloned. Copying itself invokes no
user code, constructor, destructor, or managed allocation. Backend copy elision
is permitted only when these observable semantics are preserved.

Parameters are independent value copies, including non-final parameters whose
fields the callee changes. Returning a struct produces an independent value,
never an escaping reference to local storage. Constructor and function results
are ordinary values. There are no user-defined copy/move operations or implicit
conversions between distinct structs, numeric types, enums, or `object`.

`var` retains exact type identity. Arrays remain invariant. Array literals copy
each evaluated element, left to right. `for (var point in points)` binds a copy
on each iteration; changing its fields never writes back to the array. Mixed-
struct and struct/reference array literals are invalid. Existing empty-array
literal restrictions remain.

`Point?` and `Point?[]` are invalid until nullable value types are designed.
`Point[]?` remains a nullable array reference. A struct may contain supported
nullable reference fields. Safe access that would require a nullable struct
result is diagnosed; it must not silently invent nullable value semantics.

## Storage, mutation, and final

A field can be changed through a writable storage location: a mutable local or
parameter, a writable class/struct field, or an array element. Nested inline
field paths retain their storage identity. Reading any of these as a value
copies it. Assigning a field of a temporary, such as `MakePoint().X = 3`, is
invalid; it must not silently modify a discarded copy.

`final` prevents changes to the stored struct value, including its inline
fields. It is not deep object immutability:

```cloth
final Point fixed = Point(1, 2);
fixed.X = 3;                      // Invalid: modifies the final value.

final Point[] points = [Point(1, 2)];
points[0].X = 3;                  // Valid: the array reference is final.
```

Read-only propagation follows inline struct fields and stops at a managed
reference. Thus an object or array reached through a final struct can still
be mutated, but its reference field cannot be replaced. This preserves the
existing final-reference contract.

A `final` field inside a struct is assigned only during construction. Replacing
an entire mutable struct variable with another complete value is allowed even
when the type contains final fields; it is not a write through those fields.

Mutation retains the language's left-to-right evaluation and evaluate-once
rules. The destination receiver/index is captured before the right-hand side;
only the selected field is written after the right-hand side completes. Other
fields changed during right-hand-side evaluation must not be restored from an
old whole-struct snapshot. Numeric field updates and compound assignments use
their existing type, trap, and prefix/postfix-result rules.

## Construction and initialization

Construction produces a fully initialized value. There is no implicit default
struct value, positional aggregate literal, or synthesized constructor.
`Point()` requires an accessible declared zero-argument constructor.

- Every struct instance field, including primitive fields, requires a declaration
  initializer or initialization on every constructor exit. This is stricter than
  existing primitive class-field defaults; no class behavior changes.
- Field initializers run in declaration order. Reads before initialization,
  copying or escaping incomplete `self`, and instance calls on incomplete `self`
  are rejected. A direct read of an already initialized field is allowed.
- Constructor assignments use existing direct-assignment flow rules. Branches
  and early returns must establish initialization; loop-only writes do not.
  Existing final-field single-assignment checks remain.
- An embedded struct field is initialized as a complete value. Writes to its
  subfields do not assemble an uninitialized nested value piecemeal.
- Struct locals require declaration initializers. Struct-valued class fields
  join required constructor initialization, like existing enum fields.
- Empty structs are allowed but still require an explicit constructor to create
  a value. They have one logical value and no implicit public constructor.

Storage for construction is not observable until initialization succeeds.
Zeroing GC slots before registration is a runtime safety measure, not a source
default or permission to read incomplete values.

## Instance functions: read-only receiver

An instance function receives a read-only value snapshot as `self`. The receiver
is evaluated before explicit arguments, exactly once. Its inline fields cannot
be assigned, incremented, or passed through a hidden mutable alias. Reading,
copying, returning `self`, and calling other read-only instance functions are
valid. Methods work on mutable variables, final values, and temporaries alike.

```cloth
Point point = Point(1, 2);
point = point.Moved(3, 4);         // Explicit replacement with the result.
final Point fixed = point;
println(fixed.Moved(1, 0).X);      // Reading a returned value is valid.
```

Read-only is not purity: methods may perform I/O or mutate managed objects
reached through reference fields. Explicit non-final value parameters and local
copies remain mutable. Constructors alone receive writable, incomplete `self`.
No new source qualifier, effect inference, hidden defensive-copy mutation, or
reference-return feature is introduced.

## Equality, output, and meta queries

`==` and `!=` require the same nominal struct type and compare every instance
field, including private fields, in declaration order. Each field uses its
declared type's existing equality: primitive and enum equality, string content
equality, reference identity for class/interface/array/object fields, and the
existing null rules. Embedded structs compare recursively. Static fields and
padding do not participate. Empty structs compare equal.

Both value operands are evaluated once, left to right, before field comparison.
Floating-point NaN retains existing non-reflexive equality; raw byte comparison
is not an equivalent implementation. There are no ordering operators,
struct arithmetic, custom equality hooks, or automatically synthesized hashing.

`print` and `println` accept structs without boxing and produce
`<qualified.TypeName>`, consistent with the existing default object display.
They do not expose fields, padding, storage addresses, or GC state.
`value::typeName` returns the qualified nominal name and evaluates its operand
once. Fields/methods use `.`, meta queries use `::`. Custom formatting and
field reflection remain deferred. Reference `is`/`as` and struct truthiness are
invalid.

## Aggregate layout and precise collection

Structs are inline aggregates, not heap objects. Layout follows instance-field
declaration order with target alignment and tail padding, without a managed
header, inheritance prefix, vtable, or allocation entry. Empty structs occupy
one byte with alignment one so array stride is never zero. Native layout is
not a portable serialization contract; raw bytes and packing are not exposed.

Layout dependencies follow embedded struct fields only. Direct or indirect
inline cycles are diagnosed with a cycle path. A managed class or array field
breaks that dependency: `Node[] Children` is a reference, not infinite inline
storage. Function signatures do not create layout-cycle edges.

Each aggregate layout owns a sorted, unique list of nested managed-reference
offsets, including nullable references. Class descriptors flatten offsets from
their embedded struct fields. Struct arrays need element stride, alignment,
and all per-element reference offsets; the current reference-element boolean
cannot describe them. Primitive/enum arrays have no reference offsets; ordinary
reference arrays have one at offset zero.

Live local, parameter, temporary, constructor, and result values protect every
contained reference across safepoints. The existing root-frame shape can be
retained by registering pointer-sized slots within addressable aggregate
storage. Slots start null before registration. Partial construction must keep
already initialized references alive. Copies and returns establish destination
roots before clearing source roots, including at control-flow joins.

Interior addresses are not managed object roots. A class/array owning a field
location must stay rooted across right-hand-side evaluation, calls, and any
allocation. Collection must neither miss inline references nor scan scalar
bits or padding as pointers. Tests must force collection while references are
reachable only through struct locals, nested fields, arrays, parameters, and
return paths. Struct storage itself has no independently managed identity.

## ABI and package boundary

The proposed callable ABI passes aggregate values through explicit,
target-aligned storage, rather than relying on host C++ struct ABI rules:

- each struct argument is a private value snapshot; any indirect parameter
  storage is call-scoped and cannot alias the caller's source value;
- a struct result uses a leading caller-owned result slot, followed by the
  instance receiver when present, then explicit parameters in source order;
- an ordinary struct receiver is a read-only snapshot; a constructor uses its
  result slot as writable `self` and has no heap allocation wrapper;
- caller/callee responsibilities preserve contained roots before, during, and
  after calls, including class/interface methods with struct parameters/results;
- direct and separate compilation use identical physical signatures, layout,
  alignment, receiver/result roles, and native symbol ownership.

Exact LLVM signatures, metadata fields, resource limits, and schema fixtures
must be reviewed and frozen in 26.3 before their implementation. Typed IR must
distinguish aggregate values, writable locations, and read-only views rather
than disguising structs as managed pointers. Private parameter snapshots may
be optimized only with proof that copy and lifetime semantics are unchanged.

Imported records must retain the struct kind, exact field/member identity and
visibility, initialization-relevant qualifiers, aggregate layout, complete
reference maps, and callable passing modes. Readers recompute layouts from
verified dependency declarations and reject inline cycles, overflow, wrong
offsets, missing fields, false GC maps, and signature mismatches. Private fields
remain present for layout and built-in equality without becoming accessible.

Implemented compatibility transition: artifact format **3**, compiler ABI **4**,
and runtime ABI **2**, because aggregate records/calls and array tracing cross
those boundaries. Earlier format-2/ABI-3/runtime-1 artifacts must be rebuilt,
not upgraded implicitly. Process protocol 2, receipt schema 1, and manifest
schema 1 remain unchanged.

Shuttle continues to pass opaque artifacts and validate compiler receipts.
No Rust/C++ FFI, compiler AST sharing, new build system, remote dependencies,
or general array reflection is required. Tracing metadata alone does not enable
checked array casts or expose runtime type metadata to Cloth source.

## Approval and verification gates

The 26.1 initialization, copy/final, read-only-receiver, equality, and output
rules are approved. The separate ABI/schema review gate was satisfied before
26.3 implementation; source-contract approval alone does not authorize
unreviewed incompatible encodings.

The exit suite must cover syntax/recovery, identity/import order, constructor
visibility and flow, shallow reference copying, final paths across value and
reference boundaries, nested mutation with side effects, temporary diagnostics,
array/iteration copy behavior, equality including NaN and private fields,
evaluate-once output/meta queries, inline-cycle diagnostics, aggregate calls,
and precise collection under forced-GC stress.

Negative HIR/MIR/ABI/artifact tests must reject malformed aggregate operations,
layout and root metadata, invalid receiver/result modes, and incompatible
packages. Source-free dependencies must behave like whole-project builds;
serial/parallel builds must retain deterministic artifacts and invalidation.
Both C++ configurations, native/shared-tool tests, Rust checks, and owning
documentation are required before Stage 26 is complete.
