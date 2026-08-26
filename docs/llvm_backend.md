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

## Control flow and expressions

Each MIR basic block becomes one LLVM basic block. Jumps, branches, returns,
and unreachable terminators lower directly. MIR phi values remain LLVM phi
instructions, preserving `&&` and `||` short-circuit behavior.

Integer operations select signed or unsigned division, remainder, and
comparison from the semantic operand type. Floating-point operations use LLVM
floating instructions. Null-to-reference conversions disappear because both
representations use opaque pointers.

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

Generated modules declare six runtime functions:

```llvm
declare ptr @cloth_rt_alloc(i64, i64)
declare ptr @cloth_rt_string_literal(ptr, i64)
declare void @cloth_rt_require_receiver(ptr)
declare void @cloth_rt_print(ptr)
declare void @cloth_rt_print_i32(i32)
declare void @cloth_rt_print_bool(i8)
```

`cloth_rt_alloc` receives object size and alignment and returns
zero-initialized storage or terminates through the runtime failure path.
`cloth_rt_string_literal` constructs or interns an opaque Cloth string from
immutable bytes.
`cloth_rt_require_receiver` returns for a non-null reference and traps through
the runtime null-reference path otherwise.
The print functions write a `String`, signed `int32`, or lowercase `bool`
respectively. LLVM `i1` booleans are extended to the runtime ABI's `i8`.

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
