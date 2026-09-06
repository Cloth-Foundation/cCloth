# Cloth native runtime and execution

Stage 6.0 turns a verified Cloth compilation into a native x86-64 executable.
Stage 7.0 adds initial typed output, Stage 9.0 adds fixed-length arrays, and
Stage 10.5 completes scalar output, Stage 13.1 replaces name-only runtime
descriptors with precise compiler-emitted file-class metadata, and Stage 13.2
adds the thread-local precise-root stack. Stage 13.3 adds non-moving collection
for file-class objects, Stage 13.4 extends it to strings and arrays, and Stage
13.5 adds liveness-aware roots and monotonic collector diagnostics.
Stage 14 adds immutable UTF-8 concatenation, content equality, and string meta
queries. Stage 15 adds universal object queries and checked runtime type
operations. Stage 16.2 adds parent-linked file-class descriptors and flattened
inherited reference maps. Stage 16.4 makes checked file-class operations follow
those parent links. Stage 16.5 adds immutable virtual-function tables to
file-class descriptors; generated code performs the dispatch directly. Stage
18 adds sorted interface dispatch tables and checked interface lookup without
changing the managed-reference representation. Stage 29.2 adds the checked
integer-arithmetic guard and advances the runtime ABI to 3. Stage 34.3 adds
managed error descriptors, `DivisionByZero` construction, and terminal error
reporting under runtime ABI 4. Stage 37.2 adds owned portable program arguments
and advances the runtime ABI to 5.

## Source contract

The core scope contains these intrinsic families:

```text
print(T): no value
println(T): no value
println(): no value
```

`T` covers every primitive, `object`, and `null`; file classes and arrays widen
to `object`. `print` does not add
a newline; `println` adds exactly one line feed. The complete representation
rules are in
[printing_and_object_representation.md](printing_and_object_representation.md).

A native compilation must contain exactly one eligible entry point:

```text
static func Main() { ... }
static func Main(): void { ... }
static func Main(): int32 { ... }
static func Main(string[] arguments) { ... }
static func Main(string[] arguments): void { ... }
static func Main(string[] arguments): int32 { ... }
```

Capitalization makes `Main` public. It takes either no explicit parameters or
one exact non-null `string[]` parameter with non-null elements. Omitting the
return type or explicitly returning `void` produces process status zero;
returning `int32` supplies the process status. A `Main` that declares `throws`
uses the error ABI; its LLVM adapter reports a non-null error and returns the
reporter's nonzero status. Successful completion retains the ordinary status.

## Runtime ABI

The static Cloth runtime exposes these C-linkage operation groups:

```text
cloth_rt_alloc(type_descriptor) -> reference
cloth_rt_gc_push_frame(frame, root_slots, root_count)
cloth_rt_gc_pop_frame(frame)
cloth_rt_gc_collect()
cloth_rt_gc_live_objects() -> uint64
cloth_rt_gc_live_bytes() -> uint64
cloth_rt_gc_collection_count() -> uint64
cloth_rt_gc_peak_live_bytes() -> uint64
cloth_rt_string_literal(data, size) -> string
cloth_rt_string_concat(left, right) -> string
cloth_rt_program_arguments(host_count, host_values) -> string[]
cloth_rt_string_equal(left, right) -> uint8
cloth_rt_string_length(value) -> int32
cloth_rt_string_byte_length(value) -> int32
cloth_rt_string_is_empty(value) -> uint8
cloth_rt_object_type_name(value) -> string
cloth_rt_object_is_kind(value, kind) -> uint8
cloth_rt_object_is_type(value, type_descriptor) -> uint8
cloth_rt_object_is_interface(value, interface_id) -> uint8
cloth_rt_interface_function(value, interface_id, function_slot) -> pointer
cloth_rt_array_alloc(length, element_layout) -> array
cloth_rt_array_length(array) -> int32
cloth_rt_array_element(array, index) -> element address
cloth_rt_require_receiver(reference)
cloth_rt_require_non_null(reference)
cloth_rt_require_numeric_conversion(valid)
cloth_rt_require_integer_arithmetic(valid, reason)
cloth_rt_make_division_by_zero() -> error
cloth_rt_report_error(error) -> int32
cloth_rt_print(string)
cloth_rt_print_{i8,i16,i32,i64}(signed integer)
cloth_rt_print_{u8,u16,u32,u64}(unsigned integer)
cloth_rt_print_{f32,f64}(floating point)
cloth_rt_print_char(uint32)
cloth_rt_print_bool(uint8)
cloth_rt_print_object(reference)
cloth_rt_print_newline()
```

Object allocation validates the immutable descriptor, honors its verified ABI
size and alignment, and zeroes the storage. It stores the descriptor address in
the first object-header word and an opaque allocation-registry entry in the
second. The descriptor also records object kind, a nullable direct-parent
descriptor, qualified identity, and exact reference-field offsets. Derived
descriptors contain inherited and local offsets in one ordered table. A
file-class descriptor also records an immutable virtual-function table and
count. It also records a sorted interface table whose entries contain an
interface identity, function table, and contract slot count. Allocation rejects
inconsistent tables, duplicate or unsorted identities, null function slots,
and derived descriptors that lose inherited interface metadata. Runtime
Error descriptors use the same managed header, layout checks, parent links,
reference maps, and collector traversal as file classes. The runtime owns the
root `Error` and derived `DivisionByZero` descriptors; `Error.Message` is a
managed string reference. Runtime strings and arrays begin with the same
managed header while remaining opaque to generated LLVM IR. A literal string borrows
immutable program-lifetime bytes. A concatenated string owns its separately
allocated bytes. Both cache byte and Unicode scalar lengths; collection reclaims
owned payloads together with their managed headers.

`cloth_rt_program_arguments` removes the host executable name and constructs a
managed array of owned immutable strings. Windows entry adapters receive the
wide host vector and reject unpaired UTF-16 surrogates. POSIX adapters require
strict UTF-8. Count, size, conversion, and allocation arithmetic are checked;
invalid text fails before user code with
`cloth runtime error: program argument is not valid Unicode`. The partially
constructed array remains rooted across every allocation safepoint, and the
entry adapter roots the completed array for the full `Main` invocation.

Object type-name queries return a managed immutable string over stable
program-lifetime name bytes. File classes use qualified descriptor names,
strings use `string`, and arrays use the erased name `array`. File-class checks
compare the object's canonical descriptor and then walk direct-parent links,
so every transitive base matches. String checks compare the runtime heap kind.
Null fails every concrete type check without trapping.

Interface membership uses binary search over the most-derived class
descriptor. Interface dispatch performs the same lookup, validates the
contract slot, and returns its function pointer. Generated code invokes that
pointer with the original receiver. Missing entries and invalid slots are
compiler or metadata failures and trap; source-level checked casts use the
non-trapping membership operation.

Root frames are stack-owned by generated callables and linked in thread-local
LIFO order. Each registered entry is the address of a stack slot containing a
managed reference. Push validates and links a frame without allocating; pop
requires the active frame and clears it after unlinking. The collector consumes
this internal stack during marking.

Every managed allocation, including string concatenation, is an automatic
collector safepoint. Marking uses an intrusive, non-allocating worklist,
descriptor offsets for file classes, and
per-element reference-offset scans for arrays. Strings are leaves. Sweeping
releases unmarked headers, array payloads, owned string payloads, and registry
entries. Cycles require
no special case. Explicit collection and live object/byte counters support
runtime tests and embedding diagnostics. The collector supports one Cloth
mutator and does not scan roots belonging to concurrently executing threads.
The diagnostics report current live objects/bytes, total completed collections,
and peak managed bytes. They are C ABI facilities for tests and embedders, not
Cloth source intrinsics.

An array header records its fixed length, element size, payload address, and
whether elements are references. Payload storage is zero-initialized and
properly aligned. Null access, negative indices, and indices greater than or
equal to `::length` terminate through the runtime failure path.

`cloth_rt_require_numeric_conversion` terminates with
`numeric conversion is out of range` when a compiler-emitted conversion
predicate is false. The runtime does not perform the conversion or define
wrapping behavior; source and target types remain explicit in MIR.

`cloth_rt_require_integer_arithmetic` returns without side effects for a true
predicate. Generated code still uses its overflow reason before a fixed-width
operation that cannot produce a valid result. The older division/remainder
reason values remain runtime-ABI inputs, but Stage 34.3 lowering routes an
executed zero divisor through `cloth_rt_make_division_by_zero` and the explicit
error channel instead.

`cloth_rt_report_error` validates the managed error, writes its stable type and
optional UTF-8 `Message` to standard error, and returns a nonzero status. It is
called only by the generated native entry adapter. Cloth functions propagate
the same rooted error reference without invoking the reporter.

Runtime contract violations outside the typed-error model write a concise
message to standard error and terminate the process. Typed errors use no host
exception or unwinding ABI; Stage 34 intentionally adds no local recovery.

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
An argument-taking program uses `wmain` on Windows and `main` on POSIX. Programs
with the established zero-parameter entry retain their existing native entry
symbol.
MinGW builds statically link their compiler runtime dependencies so the result
does not require toolchain-specific DLLs.

Native output currently supports x86-64. The wasm32 profile remains valid for
LLVM IR emission but does not yet have a WebAssembly runtime or linker path.

## Deferred work

Stage 15 completes the first universal managed-reference contract. Primitive
boxing, reified array casts, string indexing, slicing, iteration, formatting,
normalization, interning, root-slot reuse,
optimization levels, debug information, console input, environment access,
argument parsing, local error handling, foreign exceptions, and platform
packaging remain future work. These features should extend the runtime and
toolchain boundaries without changing existing source contracts.


## Aggregate arrays introduced in runtime ABI 2

`ClothArrayElementLayout` is `{ uint64 size, uint64 alignment,
const uint64* reference_offsets, uint64 reference_count }`. Allocation has the
physical signature `ptr(i32, ptr)`. Metadata and its offset table are immutable
program-lifetime storage referenced by the array; neither is a managed object.

The allocator rejects zero/overflowed stride, invalid alignment, mismatched
count/table pairs, and unsorted, duplicate, unaligned, or out-of-bounds reference
slots. It checks payload arithmetic and zeroes aligned storage before publication.
Scanning visits every reference slot of every element, including nested structs.
The class descriptor and root-frame shapes are unchanged. Root addresses may point
to managed-reference slots inside aggregate buffers; an aggregate address itself
must never be registered as a heap reference.
