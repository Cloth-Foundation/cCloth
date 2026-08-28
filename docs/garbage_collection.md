# Cloth Stage 13.1-13.5 managed heap

Stage 13.1 establishes the immutable heap-type metadata required by a precise
tracing collector. Stage 13.2 adds compiler-generated precise root frames.
Stage 13.3 adds a single-mutator, non-moving mark-and-sweep collector for
file-class objects. Stage 13.4 unifies strings and arrays with the same managed
heap. Stage 13.5 makes generated roots liveness-aware and adds collector
diagnostics.

## Heap object kinds

The runtime ABI reserves stable numeric kinds for file-class objects, strings,
and arrays. A kind selects the tracing strategy:

- file classes use a fixed list of object-relative reference offsets;
- strings contain no managed references;
- arrays use their element layout to determine whether their payload contains
  references.

File classes use compiler-emitted descriptors. Strings and arrays use private,
runtime-owned descriptors because their representations remain opaque to
generated code. Every kind begins with the same two-word managed header and is
registered in the same allocation index.

## File-class descriptors

Verified ABI lowering builds one `AbiTypeDescriptor` for each file class. It
contains:

- the file-class object kind;
- the qualified file-class name;
- the complete object size and alignment;
- an ordered list containing the byte offset of every reference-valued instance
  field.

Nullable references use the same pointer representation and therefore appear
in the offset list. Primitive fields and static fields do not. Offsets are
derived from the final class layout rather than source syntax, so padding and
target pointer width are already accounted for.

The ABI verifier recomputes every descriptor. Reference offsets must be unique,
strictly increasing, pointer-aligned, outside the two-word object header, and
large enough to contain one complete target pointer within the object.

## Runtime ABI

LLVM emits each descriptor as immutable global storage with this logical field
order:

```text
kind:              uint64
name:              pointer to immutable bytes
name_size:         uint64
size:              uint64
alignment:         uint64
reference_offsets: pointer to immutable uint64 offsets, or null
reference_count:   uint64
```

The explicit integer widths and target-native pointers make the same contract
usable by x86-64 and wasm32 data layouts. Kind values are fixed at `0` for file
classes, `1` for strings, and `2` for arrays.

`cloth_rt_alloc(descriptor)` validates the metadata, allocates zero-initialized
storage using its size and alignment, stores the descriptor address in the
first header word, and stores opaque collector state in the second. The string
and array allocation calls initialize the same header with their runtime-owned
descriptors. Descriptors and their referenced name and offset tables have
program lifetime. A file-class descriptor address is the canonical type
identity within the emitted module; its qualified name is stable diagnostic and
display identity.

String objects retain a pointer and length for compiler-emitted literal bytes.
Those immutable bytes already have program lifetime and are not copied or freed
with the managed string header. Array objects own a separately aligned,
zero-initialized payload. Their managed byte count includes both the header and
the logical payload size.

The compiler currently emits all discovered source files into one LLVM module.
Separate compilation will require descriptor coalescing or runtime registration
before descriptor addresses can remain canonical across module boundaries.

## Precise root frames

Each generated callable that can hold a managed reference creates one stack
frame with this logical layout:

```text
previous:   pointer to the caller's root frame
roots:      pointer to an array of root-slot addresses
root_count: uint64
```

The runtime keeps the active frame in thread-local storage. Push links a frame
to its caller; pop requires the same frame to be at the top and clears it after
unlinking. A malformed slot list or out-of-order pop terminates through the
runtime failure path. Push and pop do not allocate and are not safepoints.

Every root-array entry points to a pointer-sized stack slot, not directly to an
object. This lets a future moving collector update the slot without changing
the root-frame ABI. Stage 13 initially uses a non-moving collector, so generated
code may continue using the corresponding LLVM SSA value between safepoints.

The LLVM backend roots these categories:

- instance receivers and field-initializer receivers;
- constructor `self`, before any field initializer or constructor statement;
- reference-valued parameters and local bindings, including nullable forms;
- every reference-producing MIR value, including strings, arrays, object calls,
  nullable conversions, member loads, and reference phi nodes.

All root slots are initialized to null before registration. Reference-free
callables do not create empty frames. Every reachable return pops its frame
before returning; aborting runtime paths do not unwind because the process
terminates.

Stage 13.5 computes backward liveness for reference-valued MIR results and
reference-valued parameter/local storage. A slot is cleared immediately after
its final instruction use. At a control-flow join, generated code also clears a
slot retained only for another incoming path. A loaded reference is copied to
its temporary root before its source symbol root is cleared, and call operands
remain rooted until the call returns. Receiver and constructor `self` roots
remain callable-scoped.

## Mark and sweep

Every managed allocation has an intrusive registry entry owned by the runtime.
The object header's second word points to that entry; generated code never reads
or writes it. Registry entries record the allocation address and complete
managed size plus collector-only links and mark state.

Managed allocation is the only automatic safepoint. Specifically,
`cloth_rt_alloc`, `cloth_rt_string_literal`, and `cloth_rt_array_alloc` may
collect before reserving new storage. Checks, printing, array access, root-frame
operations, and an ordinary call boundary do not independently collect; a
callee can still reach a managed allocation safepoint. Before an allocation
would cross the current heap threshold, the runtime stops the single Cloth
mutator and runs collection:

1. Walk the thread-local root-frame chain and look up each non-null reference in
   the managed allocation registry.
2. Mark discovered objects with an intrusive worklist. The mark phase performs
   no allocation and uses no recursive C++ calls.
3. For a file class, read reference fields using the immutable offset table. A
   string is a leaf. For a reference array, read each pointer-sized payload
   element and enqueue its managed child. References not present in the registry
   are ignored safely.
4. Sweep the registry, returning unmarked headers and registry entries to the
   host allocator. Sweeping an array also releases its aligned payload. Clear
   mark state on survivors.

The uniform registry permits edges and cycles across all three kinds. Primitive
arrays do not scan their payloads; reference arrays require pointer-sized,
pointer-aligned elements so tracing never depends on source-level type recovery.

The allocation threshold has a 64 KiB floor and is recomputed as twice the
surviving managed byte count. This is a runtime tuning policy, not a source or
ABI guarantee. `cloth_rt_gc_collect()` provides an explicit runtime safepoint
for tests and embedding. `cloth_rt_gc_live_objects()` and
`cloth_rt_gc_live_bytes()` report the current heap;
`cloth_rt_gc_collection_count()` and `cloth_rt_gc_peak_live_bytes()` report
monotonic process-lifetime diagnostics. The collection counter saturates at
`uint64` maximum. Cloth source cannot invoke these functions directly.

The collector assumes one mutator. Root stacks are thread-local for ABI hygiene,
but collecting across multiple concurrently executing Cloth threads is not
supported.

## Deliberate boundaries

Stage 13.5 needs no write barrier because collection is stop-the-world,
non-generational, and only runs at explicit safepoints. Liveness is conservative
at the MIR root-slot level; it does not reuse slots or change the shadow-stack
ABI. Finalizers, weak references, concurrent tracing, generational policies,
and moving collection remain outside Stage 13.
