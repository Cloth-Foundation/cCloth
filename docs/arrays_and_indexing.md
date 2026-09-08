# Cloth Stage 9.0 arrays and indexing

Stage 9.0 adds Cloth's first homogeneous collection without committing the
language to an iterator protocol or a garbage-collector implementation.

## Source contract

An array type is written `T[]`. Arrays have fixed length, mutable elements, and
reference identity:

```cloth
int32[] values = [1, 2, 3];
values[1] = 4;
int32 count = values::length;
```

Array literals evaluate elements from left to right. Their element type starts
from the first non-null type. A compatible nullable element or `null` promotes
a reference element type, so `[user, null]` has type `User?[]`. Stage 15 joins
different managed-reference types at `object`, enabling heterogeneous
`object[]` literals without making array types covariant. Empty and
null-only literals are rejected because contextual literal typing is not yet
implemented. `null` is assignable to `T[]?`, not to non-null `T[]`. `==` and
`!=` compare array references rather than elements.

Stage 42 adds runtime-sized construction:

```cloth
int32[] offsets = int32[:count];
Token?[] tokens = Token?[:capacity];
```

The length is evaluated once and must be implicitly compatible with `int32`.
Scalar primitives receive their zero value; nullable elements receive absent.
Non-null managed references, enums, structs, and nested arrays have no
canonical default and are rejected. A constant negative length is diagnosed.
A dynamic negative length reaches the existing exact runtime failure. Stage
42.3 lowers the dedicated MIR instruction through the existing dynamic
allocator on x86-64 and wasm32 and supports native and package-artifact output.

The index type is exactly `int32`; `int` is its canonical alias. `::length` is a
case-sensitive, read-only `int32` meta query with no visibility. A nullable
array must be narrowed before indexing or meta access. Indexed reads and writes
retain runtime checks for invalid internal or foreign null references, negative
indices, and indices at or beyond `::length`.

## Compiler representation

The semantic model interns one array `TypeId` for each used element type. HIR
has explicit literal, runtime-sized construction, index, and length nodes. MIR
separates literal construction, runtime-sized allocation, load, store, and
length operations so backends do not reconstruct source syntax.

Arrays use the target's opaque reference ABI. Their element type remains
available for mangling, layout, aligned loads and stores, and collector
metadata. LLVM IR calls the runtime for allocation, length, and checked element
addresses; generated code never reads an array header directly.

## Runtime and collection

Each managed array records its element size, length, aligned payload address,
and immutable element-layout metadata with exact contained-reference offsets.
Primitive/enum payloads have empty maps; reference elements have `[0]`; structs
have flattened maps. The collector scans every listed slot in every element.
Struct element reads and iteration bindings are independent value copies, while
indexed field writes target the original element through a captured storage path. Sweeping releases both the managed array header and its
owned payload. This policy requires no change to Cloth source or MIR semantics.

## Deferred work

Stage 9.0 deliberately defers multidimensional syntax, resizable lists, slices,
array covariance, deep equality, and contextual empty literals. Stage 10.0
builds array `for` iteration on the concrete `::length` and indexed-access
semantics established here.

Checked casts to array types also remain deferred because the runtime does not
yet reify complete element-type identity.
