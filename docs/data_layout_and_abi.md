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

## Primitive representation

Fixed-width integer and floating-point names retain their declared bit widths.
`int`, `uint`, and `float` remain aliases of `int32`, `uint32`, and `float32`.
`byte`, `int8`, and `uint8` occupy one byte. `char` is a 32-bit value. `bool`
has one-bit value semantics and one-byte storage.

`void` is a callable-result marker with ABI size zero and alignment one; it has
no storage representation. An omitted function return annotation and explicit
`: void` produce the same semantic and ABI return type.

`String`, file classes, arrays, nullable wrappers, and `null` use the target
reference representation. A reference has the target pointer size and
alignment. ABI references are opaque; the contract does not expose a native
C++ object or commit the future garbage collector to non-moving addresses.
Array element types remain structural semantic data and use an `a` prefix in
mangled names. Nullable wrappers erase to the underlying reference encoding,
so `T` and `T?` have identical layouts and mangling.

`final` is a source binding contract and has no ABI representation. It does not
change field layout, parameter types, return types, overload identity, or
mangled names.

## File-class objects

Every file-class object begins with two opaque, reference-sized runtime words.
Stage 10.5 initializes the first with an opaque type descriptor and the second
with null collector state. The descriptor carries the qualified file-class
identity used by deterministic object output; generated LLVM still does not
depend on its private runtime layout.

Fields follow the header in source declaration order. Each field is aligned for
its ABI type, padding is explicit in its recorded offset, and the complete
object size is rounded to the largest required alignment. Empty file classes
still contain the two-word header.

Static fields are not object fields. Stage 12.2 records them in a separate ABI
table and emits their literal value as constant global storage. Their `_C1S`
name includes the qualified file class and field name. Static field linkage is
still determined by capitalization.

`String` remains an opaque runtime type and does not use file-class field
layout.

## Callable ABI

Every instance `func` ABI has one leading receiver slot followed by its
declared parameters. Instance-qualified calls supply their object, and
unqualified instance calls forward the current receiver. Static functions omit
the receiver slot entirely. Semantic analysis rejects class-qualified instance
calls and instance-qualified static calls, so null is never used as a stand-in
receiver for a valid function call.

Constructor entry points accept only their declared parameters and return the
new file-class reference. Backend lowering allocates the object, makes it
available as `self` to the constructor MIR body, and returns it after successful
initialization. Field initializer composition and failure behavior remain a
runtime-lowering responsibility.

Public capitalization produces external linkage. Private capitalization
produces internal linkage. Callable entry points use the target's C calling
convention so LLVM and non-Cloth tooling have a stable interoperability point.

## Mangling

Callable symbols use the ABI-versioned `_C1` prefix. The encoding includes
callable kind, length-prefixed qualified file-class and member names, parameter
count, and canonical parameter types. Return types are omitted because Cloth
does not overload on a return type. Package qualification prevents equal class
stems in different packages from producing the same native symbol.

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
