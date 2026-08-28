# Cloth native runtime and execution

Stage 6.0 turns a verified Cloth compilation into a native x86-64 executable.
Stage 7.0 adds initial typed output, Stage 9.0 adds fixed-length arrays, and
Stage 10.5 completes scalar output, Stage 13.1 replaces name-only runtime
descriptors with precise compiler-emitted file-class metadata, and Stage 13.2
adds the thread-local precise-root stack. Stage 13.3 adds non-moving collection
for file-class objects, Stage 13.4 extends it to strings and arrays, and Stage
13.5 adds liveness-aware roots and monotonic collector diagnostics.
Stage 14 adds immutable UTF-8 concatenation, content equality, and string
property queries.

## Source contract

The core scope contains these intrinsic families:

```text
print(T): no value
println(T): no value
println(): no value
```

`T` covers every primitive, every file class, and `null`. `print` does not add
a newline; `println` adds exactly one line feed. The complete representation
rules are in
[printing_and_object_representation.md](printing_and_object_representation.md).

A native compilation must contain exactly one eligible entry point:

```text
static func Main() { ... }
static func Main(): void { ... }
static func Main(): int32 { ... }
```

Capitalization makes `Main` public. It takes no explicit parameters. Omitting
the return type or explicitly returning `void` produces process status zero;
returning `int32` supplies the process status. An LLVM `main` adapter invokes
the receiver-free Cloth function directly.

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
cloth_rt_string_equal(left, right) -> uint8
cloth_rt_string_length(value) -> int32
cloth_rt_string_byte_length(value) -> int32
cloth_rt_string_is_empty(value) -> uint8
cloth_rt_array_alloc(length, element_size, element_alignment,
                     contains_references) -> array
cloth_rt_array_length(array) -> int32
cloth_rt_array_element(array, index) -> element address
cloth_rt_require_receiver(reference)
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
second. The descriptor also records object kind, qualified identity, and exact
reference-field offsets. Runtime strings and arrays begin with the same managed
header while remaining opaque to generated LLVM IR. A literal string borrows
immutable program-lifetime bytes. A concatenated string owns its separately
allocated bytes. Both cache byte and Unicode scalar lengths; collection reclaims
owned payloads together with their managed headers.

Root frames are stack-owned by generated callables and linked in thread-local
LIFO order. Each registered entry is the address of a stack slot containing a
managed reference. Push validates and links a frame without allocating; pop
requires the active frame and clears it after unlinking. The collector consumes
this internal stack during marking.

Every managed allocation, including string concatenation, is an automatic
collector safepoint. Marking uses an intrusive, non-allocating worklist,
descriptor offsets for file classes, and
pointer-element scans for reference arrays. Strings are leaves. Sweeping
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

Stage 14 completes the first immutable UTF-8 string value contract. String
indexing, slicing, iteration, formatting, normalization, interning, root-slot
reuse, optimization levels, debug information, command-line arguments,
exceptions, and platform
packaging remain future work. These features should extend the runtime and
toolchain boundaries without changing existing source contracts.
