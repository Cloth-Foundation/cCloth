# Stage 26.3: aggregate ABI and artifact review

Status: **approved, frozen, and implemented in 26.3; 26.4 exit audit pending**.

Stage 26.3's boundary contract and implementation were approved on 2026-09-02,
after clarifying that runtime metadata references introduce no source borrowing
or programmer-managed lifetimes. This document freezes the contract required by the
[roadmap](../../ROADMAP.md#stage-26-value-structs). The
[approved source contract](stage_26_structs.md) is unchanged. Native execution
and source-free packages are implemented; the owning contracts are
[structs](../structs.md), [ABI](../data_layout_and_abi.md), and
[artifact format 3](../artifact_schema_v3.md).

## Approved design

Use target-aligned inline storage, explicit value snapshots at call boundaries,
caller-owned result storage, and precise reference-offset maps. Do not allocate
a managed object for a struct or use a host C++ aggregate calling convention.

The coordinated transition is artifact format **3**, compiler ABI **4** (`_C4`
names), and runtime ABI **2**. Process protocol 2, receipt schema 1, and manifest
schema 1 remain unchanged. These versions must land together with their readers,
writers, runtime, Shuttle checks, tests, and owning documentation. Existing
artifacts must be rebuilt; there is no format-2 reinterpretation or migration.

The checked-in [review vectors](fixtures/stage_26_aggregate_abi.json) contain
exact approved type/declaration/layout records and target-layout expectations.
They are design fixtures, **not loadable `.cpa` files or proof of native support**.

## 1. Value layout

Add an aggregate ABI type kind. Its `bit_width` is zero; `storage.size` and
`storage.alignment` describe its actual representation. Preserve the distinct
nominal `struct` identity introduced by the frontend.

For a struct, start with offset zero and alignment one. Visit non-static fields
in declaration order, align each offset to its field type, append the field's
complete size, and track the largest alignment. Round the final size up to that
alignment. An empty struct has size one and alignment one. There is no object
header, parent prefix, descriptor, vtable, or allocator entry.

Resolve embedded struct layouts before their dependents with an iterative,
deterministically ordered dependency traversal. Function signatures and managed
references add no inline edges. Reject cycles before expanding reference maps.
Never propagate the existing `align_to` overflow sentinel as a usable layout:
every alignment, addition, multiplication, and offset shift must be checked.

Class fields embed a struct's full padded size. Existing class headers, base
prefixes, and the rule against reusing base tail padding do not change. Reference
fields still occupy one pointer, regardless of their referent's layout.

Each ABI type records `reference_offsets` relative to **one stored value**:

- primitive/enum/void: empty;
- managed reference, including a nullable reference: `[0]`;
- struct: the flattened, sorted, unique offsets of every contained reference.

For a field at offset `f`, append `f + r` for each reference offset `r` in its
value type. A class heap descriptor similarly flattens its complete field list,
including inherited fields, relative to the start of the heap object. A class
*reference value* still has only `[0]`; it must not inherit its object's map.

All reference offsets must be pointer-aligned and contain a complete pointer
within the enclosing value. Scalar fields and padding must never be scanned.
Padding is not source data, equality data, or portable serialization. Copies
may copy padding but must not expose it. Initial backing storage is zeroed before
registration or use, without making uninitialized source fields readable.

### Layout examples

The fixture defines `Label { byte Tag; string Text; uint32 Code; }` and
`Envelope { Label First; uint64 Count; Label Second; }` as field-shape shorthand,
not named struct syntax.

| Stored value | x86-64 size/alignment; reference offsets | wasm32 size/alignment; reference offsets |
| --- | --- | --- |
| `Data { uint64 CountTo; }` | 8/8; `[]` | 8/8; `[]` |
| Empty struct | 1/1; `[]` | 1/1; `[]` |
| `Label` | 24/8; `[8]` | 12/4; `[4]` |
| `Envelope` | 56/8; `[8, 40]` | 40/8; `[4, 28]` |
| Class `Holder { byte Lead; Envelope Value; }` (heap object) | 80/8; `[32, 64]` | 56/8; `[20, 44]` |

For `Data`, `GetCountTo()` is a uint64 load at byte offset zero of its receiver
snapshot. Its constructor stores its uint64 argument at offset zero of the
caller-owned result. Neither operation needs allocation or a GC root.

## 2. Callable ABI

Retain the target C calling convention. Use ordinary opaque LLVM pointer
parameters for compiler-owned aggregate storage; do not rely on LLVM `byval`,
target-specific struct registers, or native C++ layout. Logical Cloth types stay
in the ABI model even when their physical operand is a pointer.

Add these explicit contracts:

- `return_mode`: `void`, `direct`, or `indirect`;
- `receiver_mode`: `none`, `reference`, `readonly_value`, or `construction`;
- parameter `kind`: existing `receiver`/`explicit`, plus leading `result`;
- parameter `passing`: `direct`, `value_pointer`, or `result_pointer`.

Physical parameters are ordered: result pointer when present, receiver when
present, then explicit arguments in source order. A result parameter's logical
type is the returned struct; it is not a reference type and has no source symbol.

| Source role | Physical contract |
| --- | --- |
| Scalar or reference argument | Existing direct LLVM scalar/pointer |
| Struct argument | Pointer to a private, aligned, writable value snapshot |
| Class/interface receiver | Existing managed pointer; `reference` receiver mode |
| Struct method receiver | Pointer to a private, aligned snapshot; `readonly_value` |
| Struct result | Leading `result_pointer`; physical LLVM return is `void` |
| Struct constructor | Leading result pointer also serves as writable `self`; `construction` |
| Scalar/reference result or `void` | Existing direct LLVM result or `void` |

The caller evaluates and snapshots a struct receiver **before** explicit
arguments. It then evaluates arguments left to right, taking each struct snapshot
at that argument's evaluation point. Passing the same variable twice produces
distinct writable argument copies. The callee may use that call-scoped parameter
storage directly; no second entry copy is required.

The result buffer is distinct from receiver and argument snapshots and from
source-visible destination storage. It is initialized and registered before the
call; the result is published to its source destination only after the callee
returns. Even `p = p.Moved(...)` cannot overwrite the receiver while evaluating
arguments or constructing the result. Storage reuse/copy elision requires proof
of unchanged value, ordering, and root-lifetime behavior.

Do not attach pointer-wide LLVM `readonly`, `noalias`, or capture attributes
merely because the source receiver is read-only. Its reference fields can still
reach mutable objects. Such optimization attributes require separate proof.

Struct constructors have **one** callable entry under the existing canonical
`constructor` member domain. They have no allocating wrapper or separate base
initializer entry. Class constructors keep both entries: allocation returns the
class pointer, and initialization takes the class receiver. Struct arguments to
either class entry use the same private-snapshot convention.

Class virtual and interface slots use these exact physical signatures when they
accept or return structs. Struct returns must match nominally across overrides;
existing managed-reference return covariance remains representation preserving.
Struct methods themselves have no dispatch slots. Static `Main` remains public,
parameterless, and `void`/`int32`, with neither receiver nor result pointer.

### Physical signature examples

Descriptive LLVM names below stand for the corresponding `_C4` canonical names:

```llvm
; Data(uint64 countTo)
declare void @Data_constructor(ptr, i64)
; Data.GetCountTo(): uint64
declare i64 @Data_GetCountTo(ptr)
; Point.Moved(int32 dx, int32 dy): Point
declare void @Point_Moved(ptr, ptr, i32, i32)
; static Identity(Point value): Point
declare void @Identity(ptr, ptr)
; class/interface Transform.Apply(Point value): Point
declare void @Transform_Apply(ptr, ptr, ptr)
```

The last signature is result, managed receiver, argument snapshot. The signature
of `Identity` is result, argument snapshot, **not** receiver plus argument.

## 3. MIR values and writable paths

Preserve HIR's value/location distinction through MIR. An aggregate-producing
load, call, parameter read, array read, or phi result denotes an independent
typed value, never a writable alias to another value's storage.

Writable paths must identify a local/parameter/constructor slot or a captured
managed owner, followed by validated inline field projections and any captured
array index. Do not lower `points[i].X = value` into a load/copy of `points[i]`
followed by a store into the discarded copy. A location is compiler bookkeeping,
not a Cloth reference, and cannot escape through calls, returns, phi values, or
ordinary assignments. A read of a temporary's field is a value extraction only.

Keep owner/reference/index evaluation before the right-hand side, exactly once.
A reference-valued path edge captures the referent at that point; reassigning the
original reference during the right-hand side must not redirect the store.
Inline projections keep the original storage location even if a containing
mutable value is replaced during right-hand-side evaluation.

Only the selected field is stored after the right-hand side. Do not restore
unrelated fields from an older whole-struct snapshot. Preserve existing operation
ordering rather than silently changing it here: current `lower_assignment`
captures the location, evaluates the right-hand side, then loads the selected
field for a compound operation and stores the result. Prefix/postfix updates
load and store their captured location without a separate right-hand side.
Array/null guards stay at the corresponding read/store; capturing a deferred
location recipe alone must not move existing side effects past a trap.

MIR verification must check projection ownership/type, writable versus value
operands, exact nominal copies/phis/results, receiver modes, initialization
phases, and the managed owners required by location uses. Aggregate phis copy
the selected predecessor's value into destination storage on that edge; they
must not create pointer aliases or clear source roots before copying.

## 4. Runtime ABI 2: arrays and roots

Replace the reference-element boolean in the array allocator with an immutable,
program-lifetime element-layout record:

```cpp
struct ClothArrayElementLayout {
  std::uint64_t size;
  std::uint64_t alignment;
  const std::uint64_t* reference_offsets;
  std::uint64_t reference_count;
};

extern "C" void* cloth_rt_array_alloc(
    std::int32_t length, const ClothArrayElementLayout* element) noexcept;
```

The LLVM record is `{ i64, i64, ptr, i64 }`; the allocator declaration is
`declare ptr @cloth_rt_array_alloc(i32, ptr)`. Its metadata is compiler-emitted
constant storage, consuming-module-private, and not a managed object. The runtime
references this immutable, program-lifetime metadata. No new type-reflection or array-cast capability
is implied. Existing array headers and payload addresses remain opaque.

Validate a non-null layout pointer, nonzero stride, power-of-two alignment,
stride divisible by alignment, host-size conversions, allocation arithmetic,
and the count/table relationship. Zero references require a null table; a
nonzero count requires a table. Offsets must be strictly increasing, unique,
pointer-aligned, and within one element. A reference-bearing element's alignment
must support pointer slots. These checks validate compiler metadata, not arbitrary
native pointers supplied by hostile C code; artifacts are validated before emission.

Tracing visits `payload + index * size + reference_offset` for every element and
every listed reference. Primitive/enum arrays use an empty map, reference arrays
use `[0]`, and struct arrays use their flattened map. Check existing integer byte
helpers against an empty reference map instead of the removed boolean. Preserve
zero-length arrays, bounds checks, byte accounting, and zero-initialized payloads.

Keep `ClothTypeDescriptor`, `ClothGcRootFrame`, and root push/pop signatures
unchanged. Structs do not gain a heap kind or descriptor. A live struct slot adds
one root-slot address for every contained reference. Root entries point **into
aligned aggregate storage at reference slots**, never at aggregate/interior
addresses as if those addresses were heap objects.

Register null-initialized reference slots before any safepoint. Root receiver,
argument, construction, temporary, and result storage through their last uses.
The caller owns roots for the result slot before/during/after a call; the callee
must return a complete value before those roots can be transferred or released.
Snapshot copies and result publication happen before source slots are cleared.
Copies and root updates themselves introduce no safepoint or allocation.

Extend existing liveness to aggregate-contained references and captured managed
owners of writable paths. A class/array owner remains live across right-hand-side
evaluation and any calls/allocations until its location is consumed. Duplicate
root entries are safe; missing entries are not. Incomplete construction must
retain references initialized by earlier fields. Returning a struct never returns
a pointer into the callee's expired frame.

## 5. Equality, printing, and metadata

Evaluate and snapshot both equality operands once, left to right. Compare fields
in declaration order using declared-type operations, recursively for structs.
Use string content equality (including nullable string rules), reference identity
for class/interface/object/array fields, and existing primitive/enum equality.
Do not use whole-value `memcmp` or a same-address shortcut: padding is irrelevant
and NaN remains non-reflexive. Empty values compare equal after operand evaluation.
`!=` negates that equality result. No user-defined equality function is invoked.

Implement helpers privately in the consuming module from verified complete field
metadata, including imported private fields. Helpers do not change source member
visibility or add public symbols to artifact inventories.

`print`/`println` produce `<qualified.TypeName>` using existing string/output
runtime functions, without boxing. `::typeName` returns the existing qualified
display name, not an import alias. Operand evaluation and rooting obligations
remain intact even when the resulting display text is statically known.

## 6. Artifact format 3: exact schema delta

Inherit the envelope, canonical JSON, identity encoding, ordering, literal, and
validation rules of [format 2](../artifact_schema_v2.md) except for this section.
Unknown/missing/duplicate keys and noncanonical encodings remain errors.

### Envelope and compatibility

- Header offset 8, little-endian uint32: `3`.
- `compatibility.compiler_abi`: decimal string `"4"`.
- `compatibility.runtime_abi`: decimal string `"2"`.
- Every native Cloth name uses `_C4` plus existing canonical binary identity hex.
- Exact target/compiler/dependency/native-tool/runtime identity checks remain.
- Shuttle capabilities and receipts require artifact format `3` as a JSON number,
  not a string. No receipt, manifest, or process-protocol shape changes.

### Type records

Every type has these exact sorted keys:

```text
abi_kind bit_width display_name element id kind nominal reference_offsets storage
```

`kind` and `nominal.kind` admit `struct`. `abi_kind` admits `aggregate`.
Struct `element` is null, `bit_width` is `"0"`, and storage matches its owned
file layout. `reference_offsets` is an array of canonical decimal strings under
the value-map rules in section 1. Existing types also carry that key; it is not
optional. Nullable struct values remain invalid, while nullable struct arrays
remain valid reference types.

### Declarations and layouts

File/member/case record **keys remain exactly format 2's keys**. File kind admits
`struct`. Struct records have empty `enum_cases`, no base/conformance/interfaces,
no interface ID, no virtual/abstract functions, and false abstract/sealed flags.
Members retain visibility, final/static qualifiers, parameter names/types, and
constructor spelling. Constructors remain ordinary member records; no new keyword
or source-level receiver modifier is introduced.

The layout record retains these sorted keys:

```text
alignment callables descriptor fields header_size owner size static_fields
```

A struct has `header_size: "0"`, `descriptor: null`, and complete declaration-
ordered non-static fields, including private fields. Its layout and type record
agree on size/alignment. There is no placeholder heap descriptor for a struct.
Class descriptors and existing enum/interface placeholder encodings are unchanged;
only classes emit heap descriptor definitions. Class reference maps now flatten
struct-valued fields as well as ordinary reference fields.

Every callable has these exact sorted keys:

```text
calling_convention initializer kind linkage mangled_name member parameters
receiver_mode return_mode return_type
```

`return_type` remains the logical Cloth identity. `return_mode` and `receiver_mode`
use section 2's spellings. `parameters` is the ordered **physical** parameter list;
each entry has exactly `kind passing type`. Roles/passing modes follow section 2.
Source parameter lists in member records remain unchanged and exclude hidden slots.

Class constructor initializer records have these exact sorted keys:

```text
id linkage mangled_name parameters receiver_mode return_mode return_type
```

They always have `receiver_mode: "reference"`, `return_mode: "void"`, and the
canonical void return type. Function and struct-constructor `initializer` values
are null. Struct constructors have indirect results and construction receivers,
but **no separate receiver parameter**: their leading result slot is `self`.

The reader derives every mode and hidden slot from verified source declarations,
owner kind, and type layouts. It rejects invented or missing result slots,
aliased source storage or writable method receivers, scalar aggregate parameters, mismatched return
modes, and an allocating/base-initializer entry on a struct constructor.

### Link inventory signatures

Use one signature grammar for callable definitions, requirements, and class
constructor initializer entries. Let `T` be `_C4` plus canonical type-identity
hex; mode/role strings use the exact spellings above:

```text
c:<return_mode>:<T>(<kind>:<passing>:<T>,...);receiver:<receiver_mode>
```

The empty parameter list is `()`. No whitespace or trailing comma is permitted.
Signature reconstruction checks logical types **and** physical passing/receiver
modes, rather than assuming that every pointer-shaped operand is interchangeable.
Static-field/descriptor signature forms are otherwise unchanged except for the
new mangling prefix. Structs contribute callable/static definitions as applicable,
not descriptor or constructor-initializer definitions.

Retain conservative runtime requirements and additionally record
`cloth_rt_array_alloc` as `c:ptr(i32,ptr)` for native artifacts. The runtime ABI
version and exact runtime digest must be checked before linking. Do not accept
the old `c:ptr(i32,i64,i64,i8)` allocator against ABI 2.

### Source-free verification

An artifact may describe types owned by dependencies without owning their field
declarations. Validate local shape first, then reconstruct complete aggregate
layouts/maps/signatures against the verified dependency closure before exposing
imported structs to semantic analysis or emitting LLVM. Dependency-owned claimed
maps are not sufficient evidence by themselves.

Require every owned instance field exactly once, preserve private fields for
layout/equality, reject cross-kind identities and inline cycles, check arithmetic
before expansion, and compare reconstructed offsets/maps/signatures byte for
byte. No partial import is accepted after an error. Exact dependency digests keep
layout edits invalidating consumers through existing Shuttle rules.

## 7. Resource limits

Retain the existing 64 MiB metadata, 1 GiB object-payload, and JSON-depth-128 limits.
Add explicit aggregate limits shared by source lowering and artifact validation:

| Quantity | Maximum |
| --- | --- |
| Direct instance fields per struct | 65,536 |
| Inline struct nesting depth (leaf struct = 1) | 128 |
| Padded size of one struct value | 1,048,576 bytes |
| Flattened reference slots per value/layout | 65,536 |
| Total aggregate-map entries in one compilation/import closure | 1,048,576 |
| Aggregate backing storage plus its root-address storage in one generated callable | 262,144 bytes |

These are compiler resource policies, not raw-memory language features or a
guarantee of available runtime stack. The per-callable budget is computed after
deterministic liveness-safe slot planning, before emitting stack storage. If a
value fits its type limit but not a callable's frame budget, report that fact at
the relevant callable; do not silently heap-allocate snapshots or truncate maps.
Recursion still consumes runtime stack and is not made unbounded by this policy.

Count a struct value map once per canonical type and a class heap map once per
canonical owner across the complete verified dependency closure, regardless of
how many artifacts repeat the records. A class reference's `[0]` map is not
an aggregate map. The callable budget may use conservative distinct slots;
this stage does not require a slot-reuse optimizer.

The limits bound map expansion and generated stack use for compact but adversarial
source/artifacts. Check counts before allocation, diagnose source limits with
locations, and classify artifact violations as limit failures. Whole-project and
separate compilation must apply the same accounting; no path may bypass limits.

## 8. Implementation order and acceptance

Execute these ordered work items inside 26.3; they are not new top-level stages:

1. Add checked aggregate ABI/layout/maps and signature models with both-target
   layout and corruption tests. Retain the current native gate while incomplete.
2. Preserve aggregate values, storage paths, copies, phi edges, construction, and
   receiver snapshots in MIR, with verifier tests and liveness/root dependencies.
3. Implement runtime ABI 2 array metadata, aggregate LLVM operations, roots,
   calls/results, equality/output, and native `Data` plus forced-GC tests.
4. Implement format-3 records, source-free verification, link inventories, and
   coordinated Shuttle version handling. Freeze complete `.cpa` golden bytes
   and digests generated by the new writer; review fixture changes against this
   contract rather than merely replacing expected hashes.
5. Remove the frontend-only native/export gates only when the aggregate pipeline
   passes. Update owning implemented contracts and run both compiler suites and
   affected Rust/shared-tool/native gates before closing 26.3.

Required regressions include the user's `Data` constructor/getter, empty and
nested values, class fields, arrays/iteration copies, read-only receivers,
mutable parameter copies, result non-aliasing, RHS field mutation, private-field
equality/NaN/string/null behavior, and receiver/argument evaluation order. Force
collection with references reachable only through each aggregate storage role,
including partially initialized constructors and a captured write owner.

Runtime negatives cover malformed element maps, zero/overflowed stride, alignment,
count/table mismatches, and bounds failures. MIR/ABI/artifact negatives cover
scalarized aggregates, lost copies, invalid writable paths, missing roots/maps,
wrong callable modes, layout cycles/overflow, forged dependency layouts, and old
versions. Both target layouts must verify; native execution remains x86-64.

26.4 remains the coordinated equivalence and exit audit: source-free dependencies,
whole-project versus separate behavior, serial/parallel bytes, layout invalidation,
GC stress, and the complete compiler/Rust test matrix. Neither this review nor
26.3 alone marks all of Stage 26 complete.

## Pre-implementation review checks, 2026-09-02

The review vectors passed independent identity decoding, schema key/order and
decimal-string checks, signature reconstruction, twelve target-layout calculations,
and four array-element map calculations. LLVM `opt` with `instcombine,verify`
confirmed the six example physical declarations and all six value/heap sizes
and alignments on both target data layouts.

At the review checkpoint, the development and sanitizer suites each passed the
eleven tests
selected by `ctest --preset <preset> -R '(identity|struct|abi|artifact)'`.
Those were baseline regressions, including the then-required native struct
rejection, not aggregate implementation or format-3 acceptance tests. That
review made no compiler/runtime version or production-code changes.

## Implementation verification, 2026-09-02

The approved pipeline is implemented, and native/export gates have been removed.
All 121 development and 121 sanitizer CTests pass, including forced-GC native
execution, both target layouts, malformed aggregate metadata, and source-free
Shuttle dependencies. Full format-3 artifact lengths and SHA-256 hashes are
frozen for both target layouts, with deterministic byte-for-byte re-encoding.
The [implementation checkpoint](../testing.md#stage-263-aggregate-implementation-checkpoint)
records the compiler and Rust gates. Stage 26.4 remains open.
