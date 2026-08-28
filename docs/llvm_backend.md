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
access function. `::length` is likewise a runtime query, keeping the array header
opaque to generated code.

## Control flow and expressions

Each MIR basic block becomes one LLVM basic block. Jumps, branches, returns,
and unreachable terminators lower directly. MIR phi values remain LLVM phi
instructions, preserving `&&` and `||` short-circuit behavior.

Integer operations select signed or unsigned division, remainder, and
comparison from the semantic operand type. Floating-point operations use LLVM
floating instructions. Null-to-reference and managed-reference-to-`object`
conversions disappear because all use opaque pointers.

Canonical void functions lower to LLVM `void` regardless of whether the source
omitted the return annotation or wrote `: void`. Their fallthrough and
`return;` paths emit `ret void`. Calls to them do not create an LLVM result.
The native entry adapter translates a void `Main` completion to process status
zero.

## Calls and construction

Instance functions receive the Stage 4 receiver slot followed by explicit
parameters. Instance calls pass their receiver, and unqualified instance calls
forward the current receiver. Static functions and constructor calls have no
receiver argument; constructors return the allocated object reference.

Static scalar fields lower to constant LLVM globals using their verified ABI
names. Loads address those globals directly; they never use an object offset.

Each field initializer becomes an internal LLVM helper taking the new object as
its receiver. Constructor entry points allocate the verified class size, invoke
initializers in field declaration order, run the constructor MIR body, and
return the object.

## Runtime boundary

Generated modules declare the allocation, checking, and typed-output runtime
boundary:

```llvm
declare ptr @cloth_rt_alloc(ptr)
declare void @cloth_rt_gc_push_frame(ptr, ptr, i64)
declare void @cloth_rt_gc_pop_frame(ptr)
declare ptr @cloth_rt_string_literal(ptr, i64)
declare ptr @cloth_rt_string_concat(ptr, ptr)
declare i8 @cloth_rt_string_equal(ptr, ptr)
declare i32 @cloth_rt_string_length(ptr)
declare i32 @cloth_rt_string_byte_length(ptr)
declare i8 @cloth_rt_string_is_empty(ptr)
declare ptr @cloth_rt_object_type_name(ptr)
declare i8 @cloth_rt_object_is_kind(ptr, i64)
declare i8 @cloth_rt_object_is_type(ptr, ptr)
declare ptr @cloth_rt_array_alloc(i32, i64, i64, i8)
declare i32 @cloth_rt_array_length(ptr)
declare ptr @cloth_rt_array_element(ptr, i32)
declare void @cloth_rt_require_receiver(ptr)
declare void @cloth_rt_require_non_null(ptr)
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

`cloth_rt_alloc` receives a pointer to immutable compiler-emitted type metadata.
The descriptor supplies object kind, qualified name, verified size and
alignment, and exact reference-field offsets. The runtime returns
zero-initialized storage with that descriptor in its first header word or
terminates through the runtime failure path.
`cloth_rt_string_literal` constructs an opaque Cloth string over immutable
program-lifetime bytes. Concatenation returns a new managed string with owned
bytes. Equality compares byte content, and meta-query calls expose cached scalar
and byte lengths without revealing the runtime layout.
Object widening is pointer-preserving. `::typeName` calls the runtime and
returns a managed string. Exact file-class checks pass the compiler-emitted
descriptor; string checks pass the stable heap-kind value. Safe `as T?`
lowering selects the original pointer or null from the resulting predicate.
The array calls allocate typed element storage, query its fixed length, and
perform null and bounds checked element addressing.
`cloth_rt_require_receiver` returns for a non-null reference and traps through
the runtime null-reference path otherwise.
The print functions cover all primitive ABI widths, file-class references, and
line feeds. LLVM `i1` booleans are extended to the runtime ABI's `i8`.

These declarations intentionally keep allocation, strings, traps, and
collector mechanics outside generated user functions. Stage 13.1 emits
descriptor globals. Stage 13.2 emits pointer-precise shadow-stack frames for
receivers, constructor `self`, reference parameters and locals, and every
reference-valued MIR result. Slots are initialized before registration and each
reachable return unregisters its frame. Push and pop are not safepoints.

Stage 13.5 performs backward reference liveness over the MIR CFG. It clears
temporary and parameter/local root slots after their last use and clears values
retained only by another predecessor when control flow joins. A collecting call
sees all of its reference operands rooted; result roots are populated before
any source root is cleared. Managed allocation calls, including string
concatenation and `::typeName`, are the only automatic safepoints. Runtime
checks, output, array access, and shadow-stack maintenance do not collect.

## Verification and deferred work

The backend rejects inconsistent MIR and ABI inputs. CTest emits a real module
and, when available, runs:

```sh
opt -passes=verify -disable-output cloth-example.ll
```

Stage 5.0 itself does not define optimization pipelines, debug information, or
exception handling. Stage 13 defines the collector safepoint and root-liveness
contract. Stage 6.0 supplies the runtime definitions and external native
object/link pipeline described in [native_runtime.md](native_runtime.md).
