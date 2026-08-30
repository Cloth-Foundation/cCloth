# Cloth Stage 4.0 data layout and ABI

Stage 4.0 lowers verified MIR declarations into a deterministic ABI model. The
model contains no LLVM C++ types, but its sizes, alignments, signatures,
linkage, and mangled names are sufficient for a future LLVM backend. This keeps
the front end inexpensive to build and lets LLVM remain a replaceable backend
boundary.

## Targets

`TargetDataLayout` records target endianness, pointer size and alignment,
64-bit integer and floating-point alignment, and the object-header word count.
The compiler currently provides two LLVM-oriented layouts:

- `x86_64-unknown-unknown`: 64-bit pointers
- `wasm32-unknown-unknown`: 32-bit pointers

The command-line driver selects them with `--target=x86_64` and
`--target=wasm32`. Source types keep the same meaning on both targets; only
their target representation changes.

Target endianness describes native storage for ABI and backend validation; it
does not change arithmetic, assignment, or numeric-conversion semantics. The
integer binary-data surface encodes and decodes fixed-width integers only with
an explicitly requested little-endian or big-endian order. It does not expose
raw object storage or make “native endian” portable source behavior. See
`integer_binary_data.md`.

## Primitive representation

Fixed-width integer and floating-point names retain their declared bit widths.
`int`, `uint`, and `float` remain aliases of `int32`, `uint32`, and `float32`.
`byte`, `int8`, and `uint8` occupy one byte. `char` is a 32-bit value. `bool`
has one-bit value semantics and one-byte storage.

`void` is a callable-result marker with ABI size zero and alignment one; it has
no storage representation. An omitted function return annotation and explicit
`: void` produce the same semantic and ABI return type.

`string`, `object`, file classes, arrays, nullable wrappers, and `null` use the target
reference representation. A reference has the target pointer size and
alignment. ABI references are opaque; the contract does not expose a native
C++ object or commit the future garbage collector to non-moving addresses.
Array element types remain structural semantic data and use an `a` prefix in
mangled names. Nullable wrappers erase to the underlying reference encoding,
so `T` and `T?` have identical layouts and mangling.
Widening a managed reference to `object` is also representation preserving.
The canonical mangling code for an explicitly declared `object` parameter is
`o`; a concrete source type keeps its own encoding.

Stage 16.4 derived-to-base widening is likewise representation preserving.
Because the complete base layout begins at byte zero, neither non-null nor
nullable upcasts adjust the pointer. Callable mangling still records the
declared base parameter type; the conversion exists only in typed MIR and
erases at the LLVM boundary.

`final` is a source binding contract and has no ABI representation. It does not
change field layout, parameter types, return types, overload identity, or
mangled names.

## File-class objects

Every file-class object begins with two opaque, reference-sized runtime words.
The first points to immutable compiler-emitted type metadata. Stage 13.3 uses
the second for an opaque managed-allocation registry entry. The descriptor
carries the qualified file-class identity, complete object size and alignment,
object kind, the final ABI offset of every reference-valued instance field, and
the class virtual-function table and slot count. Stage 18 appends a sorted array
of interface dispatch entries. Each entry contains a deterministic interface
identity, a function-pointer table in contract-slot order, and its slot count.

Fields follow the header in source declaration order. Each field is aligned for
its ABI type, padding is explicit in its recorded offset, and the complete
object size is rounded to the largest required alignment. Empty file classes
still contain the two-word header.

Stage 16.2 lays out a derived file class by copying the complete, padded base
layout as its prefix and appending local instance fields. Base tail padding is
not reused. The flattened derived field table therefore begins with exactly the
base symbols, types, and offsets, and its descriptor points to the direct base
descriptor. Layout lowering resolves bases before dependents even when source
files arrive in the opposite order.

Nullable references appear in descriptor reference-offset tables because their
ABI representation is still a pointer. Primitive and static fields do not.
Descriptor verification recomputes these tables from the final flattened class
layout, so a derived table covers inherited and local references.
See [garbage_collection.md](garbage_collection.md) for the Stage 13.1 contract.

Static fields are not object fields. Stage 12.2 records them in a separate ABI
table and emits their literal value as constant global storage. Their `_C1S`
name includes the qualified file class and field name. Static field linkage is
still determined by capitalization.

`string` and arrays remain opaque runtime types and do not use file-class field
layout. Their private runtime representations begin with the same two-word
managed header, but generated code accesses them only through runtime calls.
Interfaces have no object layout or allocatable type descriptor. Interface
references retain the ordinary managed pointer representation. A conforming
class descriptor flattens every inherited interface entry, and a derived class
rebuilds each function table from its final virtual implementations. Interface
conversions therefore require no pointer adjustment or wrapper allocation.

String references retain identical ABI shape regardless of whether their UTF-8
bytes are borrowed literal storage or an owned concatenation buffer. Lengths
cross the runtime boundary as `int32`; equality results cross as `uint8` and
lower to Cloth `bool` values.

## Callable ABI

Every instance `func` ABI has one leading receiver slot followed by its
declared parameters. Instance-qualified calls supply their object, and
unqualified instance calls forward the current receiver. Static functions omit
the receiver slot entirely. Semantic analysis rejects class-qualified instance
calls and instance-qualified static calls, so null is never used as a stand-in
receiver for a valid function call.

Every public instance function receives a stable virtual slot. A derived ABI
copies its base table, replaces override implementations in place, and appends
new public instance functions in declaration order. Override parameters remain
exact, while managed-reference returns may narrow covariantly without changing
their pointer ABI. Every implementation in one slot therefore has a compatible
callable ABI. Generated virtual calls load the slot from the
most-derived descriptor; private and static calls retain their direct mangled
target. Calls on the object under construction are also direct until its field
and constructor initialization completes.

An interface call retains its static interface identity and contract slot in
MIR. Generated code resolves the matching class-descriptor entry, loads the
function pointer, and invokes it with the unchanged receiver. ABI verification
checks identity ordering, contract table lengths, and implementation symbol
kinds in addition to the existing class-layout invariants.

Each constructor has an allocation entry and an internal initialization entry.
The `_C1C` allocation entry accepts only declared parameters, returns the new
file-class reference, allocates the complete most-derived object, and runs its
constructor MIR. Its linkage is external for the uppercase class-name spelling
and internal for lowercase or underscore-prefixed private spellings.
The `_C1I` initialization entry accepts a leading `self` slot followed by the
same parameters and returns `void`. A derived constructor calls the selected
accessible base `_C1I` entry on the same object before initializing its local
fields. No base-chain step allocates another object, and the most-derived
descriptor remains installed throughout construction. Field initializer
composition and failure behavior remain a backend-lowering responsibility.

Public capitalization produces external linkage. Private capitalization
produces internal linkage. Callable entry points use the target's C calling
convention so LLVM and non-Cloth tooling have a stable interoperability point.

## Mangling

Callable symbols use the ABI-versioned `_C1` prefix. The encoding includes
callable kind, length-prefixed qualified file-class and member names, parameter
count, and canonical parameter types. Return types are omitted because Cloth
does not overload on a return type. Package qualification prevents equal class
stems in different packages from producing the same native symbol.

Constructor allocation and initialization entries differ only in their callable
kind code (`C` and `I`). Their shared suffix therefore identifies the same
source constructor unambiguously while keeping the internal entry outside the
source-level overload set.

For example:

```text
func Pick(int32 value): int32
_C1F6_Layout4_PickP1_i32
```

The ABI verifier reconstructs canonical layouts and signatures from MIR and the
semantic model, checks target validity and field bounds, and rejects duplicate
mangled names.

## LLVM boundary

The Stage 5 backend translates this model to an LLVM data-layout string, opaque
pointer types, function types, and linkage. It does not recompute Cloth field
offsets or symbol names independently. Runtime allocation, collector
safepoints, exception behavior, and platform object-file emission remain
outside the Stage 4 contract. Stage 6 supplies the initial runtime and native
object pipeline. See [llvm_backend.md](llvm_backend.md) and
[native_runtime.md](native_runtime.md).
