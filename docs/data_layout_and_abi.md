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
`int` and `uint` remain aliases of `int32` and `uint32`. `byte`, `int8`, and
`uint8` occupy one byte. `char` is a 32-bit value. `bool` has one-bit value
semantics and one-byte storage.

`String`, file classes, and `null` use the target reference representation. A
reference has the target pointer size and alignment. ABI references are opaque;
the contract does not expose a native C++ object or commit the future garbage
collector to non-moving addresses.

## File-class objects

Every file-class object begins with two opaque, reference-sized runtime words.
They reserve stable space for type metadata and runtime or collector state
without defining their backend implementation.

Fields follow the header in source declaration order. Each field is aligned for
its ABI type, padding is explicit in its recorded offset, and the complete
object size is rounded to the largest required alignment. Empty file classes
still contain the two-word header.

`String` remains an opaque runtime type and does not use file-class field
layout.

## Callable ABI

Every ordinary `func` ABI has one leading, nullable receiver slot followed by
its declared parameters. Instance-qualified calls supply their object.
Class-qualified calls supply a null receiver. Unqualified calls forward the
current receiver. Keeping the slot in every function signature gives each
overload one stable native type while Cloth has no separate `static` modifier.
Accessing instance state through a null receiver follows Cloth's null-reference
trap path.

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
callable kind, length-prefixed file-class and member names, parameter count, and
canonical parameter types. Return types are omitted because Cloth does not
overload on a return type.

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
