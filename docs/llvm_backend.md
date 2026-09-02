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
and unreachable terminators lower directly. Scalar MIR phi values remain LLVM phi
instructions, preserving `&&` and `||` short-circuit behavior. Aggregate phis use
two-phase copies on split incoming edges to preserve simultaneous assignment.

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
forward the current receiver. Static functions and allocating constructor calls
have no receiver argument; class constructors return the allocated object reference.
The constructor initializer instead receives the existing object as
its leading `self` argument and returns `void`.

Static scalar fields lower to constant LLVM globals using their verified ABI
names. Loads address those globals directly; they never use an object offset.

Each field initializer becomes an internal LLVM helper taking the new object as
its receiver. An allocating constructor entry allocates the verified
most-derived class size, installs that class's descriptor, executes the
constructor MIR, and returns the object. Its LLVM linkage follows the
constructor declaration's capitalization. Every class constructor also has an
initializer entry that executes the same MIR on an incoming `self` without
allocating. Accessible constructor initializers have external linkage for base
calls across packages; private constructor initializers remain internal. A
derived constructor invokes the selected accessible base initializer first;
the MIR field marker then invokes only the derived class's local field helpers
in declaration order before its body continues.

Class descriptors use their ABI-4 canonical external symbols rather than local
file indices. The internal package selector emits only the selected package's
definitions and declares dependency descriptors and accessible members. It
rejects entry-wrapper generation in package modules. This consumes verified source and/or source-free artifact declarations through
the shared compile/link protocol. See
[canonical identity](canonical_identity.md).

## Enum lowering

Verified enum values lower to `i32`, preserving nominal checks in HIR/MIR.
Fields, arrays, copies, equality, and calls use the ordinary scalar path.
Each used enum output type receives a module-private case-name table and print
helper, derived from source or verified imported declarations. The helper
checks the unsigned tag against the case count before indexing, trapping with
`llvm.trap` on an invalid value. It uses existing string-literal and print
runtime entries. No enum allocation, GC roots, external name-table symbols,
or runtime-ABI change is needed. Enum `::typeName` uses its static nominal name
after evaluating the operand exactly once.

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
declare i8 @cloth_rt_object_is_interface(ptr, i64)
declare ptr @cloth_rt_interface_function(ptr, i64, i64)
declare ptr @cloth_rt_array_alloc(i32, i64, i64, i8)
declare i32 @cloth_rt_array_length(ptr)
declare ptr @cloth_rt_array_element(ptr, i32)
declare void @cloth_rt_require_receiver(ptr)
declare void @cloth_rt_require_non_null(ptr)
declare void @cloth_rt_require_numeric_conversion(i8)
declare float @llvm.trunc.f32(float)
declare double @llvm.trunc.f64(double)
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
The descriptor supplies object kind, a nullable direct-parent descriptor,
qualified name, verified size and alignment, and exact reference-field
offsets. Stage 16.2 derived descriptors contain the complete inherited and
local reference map. Stage 16.5 descriptors also point to a constant array of
function pointers with one entry per stable virtual slot. The runtime returns
zero-initialized storage with that descriptor in its first header word or
terminates through the runtime failure path.
Stage 18 descriptors additionally point to a sorted array of interface entries.
Each entry contains the deterministic interface identity, a constant function
table in contract-slot order, and the table length. Interface declarations do
not emit allocatable class descriptors.
`cloth_rt_string_literal` constructs an opaque Cloth string over immutable
program-lifetime bytes. Concatenation returns a new managed string with owned
bytes. Equality compares byte content, and meta-query calls expose cached scalar
and byte lengths without revealing the runtime layout.
Checked numeric conversion passes an emitted range predicate to
`cloth_rt_require_numeric_conversion` before executing a potentially invalid
LLVM conversion. Integer narrowing uses `trunc`, integer/floating conversion
uses `sitofp`, `uitofp`, `fptosi`, or `fptoui`, and floating narrowing uses
`fptrunc`. Floating-to-integer lowering first applies the matching `llvm.trunc`
intrinsic so the range predicate follows Cloth's truncate-toward-zero contract.
Object widening is pointer-preserving. `::typeName` calls the runtime and
returns a managed string. Stage 16.4 base widening is equally pointer-preserving
because the base object is a byte-zero prefix. File-class checks pass the
compiler-emitted target descriptor, and the runtime walks from the object's
most-derived descriptor through its parent links. String checks pass the stable
heap-kind value. Safe `as T?` lowering selects the original pointer or null from
the resulting predicate.
Interface checks pass the deterministic interface identity to the runtime.
Interface calls pass that identity and the statically selected contract slot,
receive the matching implementation pointer, and call it with the unchanged
managed receiver.
Virtual calls null-check the receiver, load its most-derived descriptor, load
the descriptor's virtual table, and indirectly call the selected slot. Direct
calls remain for static and private functions and for calls on the object under
construction from its field-initializer or constructor body. Calls on unrelated
receivers inside those bodies remain virtual.

Base-qualified calls use the same current receiver pointer but call the
statically selected base ABI symbol directly. They perform no descriptor or
virtual-slot load and do not adjust the pointer because the base object begins
at byte zero.

The array calls allocate typed element storage, query its fixed length, and
perform null and bounds checked element addressing.
`cloth_rt_require_receiver` returns for a non-null reference and traps through
the runtime null-reference path otherwise.
The print functions cover all primitive ABI widths, file-class references, and
line feeds. LLVM `i1` booleans are extended to the runtime ABI's `i8`.

Contextual integer literals use the complete signed or unsigned range selected
by semantic analysis. Contextual decimal literals are emitted after one
conversion to their selected IEEE-754 width. Typed numeric widening lowers to
`sext` for signed integers, `zext` for unsigned integers, and `fpext` for
`float32` to `float64`; it is never erased as a representation-preserving cast.

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


## Aggregate lowering

A struct SSA value is represented internally by an independently owned aligned
byte buffer, not a managed object pointer. Copies use overlap-safe LLVM memory
operations; equality compares declared fields, never padding bytes or buffer
addresses. Private fields participate, strings compare by contents, other
references by identity, and NaN remains non-reflexive.

Physical parameters and results follow [ABI 4](data_layout_and_abi.md). Buffers
and their contained reference slots follow the precise-root contract. Struct
printing emits `<qualified.TypeName>` without boxing; type-name meta queries
retain that identity. Both evaluate the operand once.

Array allocation references immutable `{ i64, i64, ptr, i64 }` element metadata,
with target-aligned size and flattened reference offsets. Native execution is
x86-64; emitted aggregate LLVM is also verified for wasm32.
