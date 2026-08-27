# Cloth Stage 5.0 LLVM IR backend

Stage 5.0 lowers verified MIR and ABI data to deterministic textual LLVM IR.
The emitter uses LLVM's opaque-pointer representation and produces modules for
the selected x86-64 or wasm32 target profile.

The compiler core does not link LLVM C++ libraries. This avoids coupling Cloth
to the host toolchain's C++ ABI and keeps MinGW, Clang, and MSVC builds viable.
When LLVM `opt` is installed, the test suite runs LLVM's module verifier over
compiler output.

## Command line

`--emit-llvm` writes only LLVM IR to standard output. An explicit output path
uses `--emit-llvm=<path>`. Target selection composes with emission:

```sh
clothc --target=wasm32 --emit-llvm=program.ll Program.co
```

Invalid source or failed lowering never writes a partial module.

## Type and storage lowering

ABI integer widths lower to matching LLVM integer types. `float32` and
`float64` lower to `float` and `double`. References lower to opaque `ptr`.
Locals and explicit parameters receive entry-block storage so assignment and
lexical shadowing preserve MIR behavior. Loads and stores retain verified ABI
alignment.

Field access uses byte-addressed `getelementptr i8` with the exact offset from
the Stage 4 class layout. LLVM does not independently reconstruct Cloth object
layout.

Arrays lower as opaque pointers. Literal allocation passes the element count,
verified ABI size and alignment, and reference-content metadata to the runtime.
Every indexed load or store obtains its address through the checked runtime
access function. `Length` is likewise a runtime query, keeping the array header
opaque to generated code.

## Control flow and expressions

Each MIR basic block becomes one LLVM basic block. Jumps, branches, returns,
and unreachable terminators lower directly. MIR phi values remain LLVM phi
instructions, preserving `&&` and `||` short-circuit behavior.

Integer operations select signed or unsigned division, remainder, and
comparison from the semantic operand type. Floating-point operations use LLVM
floating instructions. Null-to-reference conversions disappear because both
representations use opaque pointers.

Canonical void functions lower to LLVM `void` regardless of whether the source
omitted the return annotation or wrote `: void`. Their fallthrough and
`return;` paths emit `ret void`. Calls to them do not create an LLVM result.
The native entry adapter translates a void `Main` completion to process status
zero.

## Calls and construction

Ordinary functions receive the Stage 4 receiver slot followed by explicit
parameters. Instance calls pass their receiver, unqualified calls forward the
current receiver, and class-qualified calls pass null. Constructor calls have
no receiver argument and return the allocated object reference.

Each field initializer becomes an internal LLVM helper taking the new object as
its receiver. Constructor entry points allocate the verified class size, invoke
initializers in field declaration order, run the constructor MIR body, and
return the object.

## Runtime boundary

Generated modules declare the allocation, checking, and typed-output runtime
boundary:

```llvm
declare ptr @cloth_rt_alloc(i64, i64, ptr, i64)
declare ptr @cloth_rt_string_literal(ptr, i64)
declare ptr @cloth_rt_array_alloc(i32, i64, i64, i8)
declare i32 @cloth_rt_array_length(ptr)
declare ptr @cloth_rt_array_element(ptr, i32)
declare void @cloth_rt_require_receiver(ptr)
declare void @cloth_rt_print(ptr)
declare void @cloth_rt_print_char(i32)
declare void @cloth_rt_print_i8(i8)
declare void @cloth_rt_print_i16(i16)
declare void @cloth_rt_print_i32(i32)
declare void @cloth_rt_print_i64(i64)
declare void @cloth_rt_print_u8(i8)
declare void @cloth_rt_print_u16(i16)
declare void @cloth_rt_print_u32(i32)
declare void @cloth_rt_print_u64(i64)
declare void @cloth_rt_print_f32(float)
declare void @cloth_rt_print_f64(double)
declare void @cloth_rt_print_bool(i8)
declare void @cloth_rt_print_object(ptr)
declare void @cloth_rt_print_newline()
```

`cloth_rt_alloc` receives object size, alignment, and qualified type-name bytes.
It returns zero-initialized storage with an active object descriptor or
terminates through the runtime failure path.
`cloth_rt_string_literal` constructs or interns an opaque Cloth string from
immutable bytes.
The array calls allocate typed element storage, query its fixed length, and
perform null and bounds checked element addressing.
`cloth_rt_require_receiver` returns for a non-null reference and traps through
the runtime null-reference path otherwise.
The print functions cover all primitive ABI widths, file-class references, and
line feeds. LLVM `i1` booleans are extended to the runtime ABI's `i8`.

These declarations intentionally keep allocation, strings, traps, and future
garbage-collector integration outside generated user functions.

## Verification and deferred work

The backend rejects inconsistent MIR and ABI inputs. CTest emits a real module
and, when available, runs:

```sh
opt -passes=verify -disable-output cloth-example.ll
```

Stage 5.0 itself does not define optimization pipelines, debug information,
exception handling, or collector safepoints. Stage 6.0 supplies the runtime
definitions and external native object/link pipeline described in
[native_runtime.md](native_runtime.md).
