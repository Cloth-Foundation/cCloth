# Cloth native runtime and execution

Stage 6.0 turns a verified Cloth compilation into a native x86-64 executable.
It keeps LLVM behind an external tool boundary and gives the first core
operation, `print(String)`, a typed language and runtime contract. Stage 7.0
extends that contract with exact `int32` and `bool` output. Stage 9.0 adds
fixed-length array allocation and checked access.

## Source contract

The core scope contains this intrinsic signature:

```text
print(String): no value
print(int32): no value
print(bool): no value
```

`print` does not add a newline. Strings are written byte-for-byte, integers use
base 10, and booleans use lowercase `true` or `false`. Ordinary local and member
lookup precedes the core scope, so source code can shadow the intrinsic
overloads without a special parser rule.

A native compilation must contain exactly one eligible entry point:

```text
func Main() { ... }
func Main(): int32 { ... }
```

Capitalization makes `Main` public. It takes no explicit parameters. Omitting
the return type produces process status zero; returning `int32` supplies the
process status. An LLVM `main` adapter invokes the Cloth function with a null
class receiver, matching ordinary class-qualified call semantics.

## Runtime ABI

The static Cloth runtime implements nine C-linkage functions:

```text
cloth_rt_alloc(size, alignment) -> reference
cloth_rt_string_literal(data, size) -> String
cloth_rt_array_alloc(length, element_size, element_alignment,
                     contains_references) -> array
cloth_rt_array_length(array) -> int32
cloth_rt_array_element(array, index) -> element address
cloth_rt_require_receiver(reference)
cloth_rt_print(String)
cloth_rt_print_i32(int32)
cloth_rt_print_bool(uint8)
```

Object allocation honors the verified ABI size and alignment and zeroes the
storage. A runtime string is currently an opaque byte pointer and length. Its
representation is deliberately absent from generated LLVM IR so interning and
garbage collection can replace the initial allocation strategy later.

An array header records its fixed length, element size, payload address, and
whether elements are references. Payload storage is zero-initialized and
properly aligned. Null access, negative indices, and indices greater than or
equal to `Length` terminate through the runtime failure path.

Runtime contract violations write a concise message to standard error and
terminate the process. There is no recovery or exception ABI yet.

## Native toolchain pipeline

`clothc --build=<path>` performs these steps:

1. Compile and verify the source through MIR and ABI.
2. Emit LLVM IR with the native entry adapter.
3. Invoke LLVM `llc` to create a host-compatible object file.
4. Invoke the C++ linker used to build Cloth and link the static runtime.
5. Remove the temporary LLVM IR and object files.

Processes are launched directly rather than through a command shell, so source
and output paths are not interpreted as shell syntax. Tool paths and the
native target triple come from the CMake configuration.
MinGW builds statically link their compiler runtime dependencies so the result
does not require toolchain-specific DLLs.

Native output currently supports x86-64. The wasm32 profile remains valid for
LLVM IR emission but does not yet have a WebAssembly runtime or linker path.

## Deferred work

The initial runtime intentionally has process-lifetime allocation. Garbage
collection, deallocation, string interning, optimization levels, debug
information, command-line arguments, exceptions, and platform packaging remain
future stages. These features should extend the runtime and toolchain
boundaries without changing the existing source contracts.
