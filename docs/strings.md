# Cloth Stage 14 string values

Stage 14 makes `string` Cloth's built-in immutable text value. The lowercase
spelling is intentional: it groups `string` with language-provided types such
as `int32` and `bool`, while uppercase names remain available to user-defined
file classes. `String` is not an alias; when no user type with that name is
visible, the compiler recommends `string`.

## Source contract

A `string` is a managed, non-null reference by default. `string?` adds `null`
through the normal nullable-reference rules. String literals and concatenation
produce `string` values.

```cloth
string greeting = "Hello, " + "Cloth";
bool same = greeting == "Hello, Cloth";
bool different = greeting != "other";
```

Strings are immutable. `+` creates a new value and leaves both operands
unchanged. `==` and `!=` compare UTF-8 byte content, not managed-object
identity. Nullable string equality additionally treats two null references as
equal and a null/non-null pair as unequal. No implicit conversion from another
type to `string` is performed by concatenation.

String literals must decode to well-formed UTF-8. Malformed input is a compile
error. Cloth preserves decoded code points exactly: it does not normalize
Unicode, so canonically equivalent but differently encoded scalar sequences
remain different values.

## Meta operations

Strings expose three language-defined, read-only meta queries:

- `value::length: int32` counts Unicode scalar values;
- `value::byteLength: int32` counts UTF-8 code units;
- `value::isEmpty: bool` is true exactly when `byteLength` is zero.
- `value::typeName: string` is the universal object query and returns `string`.

Embedded U+0000 is ordinary string content. It contributes one to both lengths
and does not terminate printing or comparison. A direct meta query requires a
non-null `string`. Stage 41 adds safe queries such as `value?::length`; absence
produces a nullable query result without evaluating the operation.

`::` is distinct from declared member access. Meta names are supplied by the
language, use lower camel case, have no visibility, and cannot be shadowed,
overloaded, or assigned. Queries are values and cannot be called. Stage 40 adds
the callable `value::slice(start, end)` operation, which returns the half-open
Unicode-scalar interval as a new immutable string. A future declared operation
such as `value.Contains("x")` continues to use `.` and ordinary member rules.

## Representation and collection

Generated code treats strings as opaque target-width references. Runtime string
objects share the managed two-word header and are leaf objects for tracing.
They cache both byte length and scalar count.

Literal objects borrow immutable bytes from compiler-emitted program-lifetime
storage. Concatenated and sliced objects own separately allocated byte buffers.
The collector accounts for owned payload bytes and releases them when the
string becomes unreachable. Literal construction, concatenation, and slicing
are managed allocation safepoints, so generated code keeps every reference
operand rooted until the call returns.

## Deliberate boundaries

The primitive representation did not itself imply traversal or slicing.
[Stage 39](proposals/stage_39_unicode_string_traversal.md) separately added
Unicode-scalar indexing and iteration. [Stage 40](proposals/stage_40_unicode_string_slicing.md)
adds `value::slice(start, end)` with checked half-open scalar bounds through
runtime ABI 8.

Interpolation, formatting, case conversion, normalization, searching,
interning, and a source-defined standard-library class remain outside this
contract. They can be layered on immutable UTF-8 strings without exposing the
runtime representation.
