# Cloth package artifact schema, version 3

This is the frozen Stage 26 `.cpa` contract, superseded by
[version 4](artifact_schema_v4.md). It inherits the frozen
[version-2 schema](artifact_schema_v2.md) and its version-1 rules except for the
changes below. Exact-key validation, canonical ordering/encoding, integrity,
tool identity, payload, and source/dependency rules remain mandatory.

## Compatibility

Header offset 8 contains little-endian format integer 3.
`compatibility.compiler_abi` is `"4"`; `runtime_abi` is `"2"`.
Canonical native names use `_C4`. Formats 1/2 and old compiler/runtime ABIs are
rejected: rebuild them, with no reinterpretation or migration.

Shuttle advertises/requires artifact format 3 as an ordinary JSON integer in
capabilities and receipts. Process protocol 2, receipt schema 1, and manifest
schema 1 are unchanged. Shuttle does not decode compiler metadata.

## Type records

Every type has exactly these sorted keys:

```text
abi_kind bit_width display_name element id kind nominal reference_offsets storage
```

Semantic and nominal kinds admit `struct`. A struct has nominal identity, null
`element`, `abi_kind: "aggregate"`, and `bit_width: "0"`. Storage is the padded
inline size and alignment, including a 1-byte size/alignment for empty structs.

`reference_offsets` is an array of canonical nonnegative decimal strings:
empty for scalar/enum/void values, `["0"]` for managed references, and flattened
contained-reference slots for structs. Maps are sorted, unique, pointer-aligned,
and contain complete pointers within value storage. Nullable structs are invalid.
An array of structs remains a managed reference and may itself be nullable.

## Declarations and layouts

File/member record keys are unchanged from version 2. File `kind` admits
`struct`; its complete private members and declaration order are retained.
A struct has no base, interfaces, dispatch tables, abstract/sealed flags, or enum
cases. Ordinary capitalization controls all member visibility.

Layout keys remain:

```text
alignment callables descriptor fields header_size owner size static_fields
```

A struct has `descriptor: null`, `header_size: "0"`, and complete ordered
instance fields with exact type/offset records. Size and alignment agree with
its nominal type record. It emits no descriptor symbol. Enum/interface
placeholder descriptors retain their existing representations.

Class heap descriptor maps now include references nested in inline fields;
the class value's `["0"]` map is distinct from its heap descriptor map.
Aggregate static constants remain unsupported.

## Callable records

Every callable has exactly:

```text
calling_convention initializer kind linkage mangled_name member parameters
receiver_mode return_mode return_type
```

`return_type` is logical source type. `return_mode` is `void`, `direct`, or
`indirect`; `receiver_mode` is `none`, `reference`, `readonly_value`, or
`construction`. Physical parameter records have exactly:

```text
kind passing type
```

`kind` is `result`, `receiver`, or `explicit`; `passing` is `direct`,
`value_pointer`, or `result_pointer`. Parameter order is result, receiver,
then explicit arguments. Struct results use a leading `result_pointer`;
explicit struct arguments use independent writable `value_pointer` buffers.
Instance struct receivers use read-only snapshots through `value_pointer`.
The logical struct result corresponds to an LLVM `void` return.

Struct constructors have construction mode and use the result buffer as self;
`initializer` is null. Class constructors retain their separate initializer:

```text
id linkage mangled_name parameters receiver_mode return_mode return_type
```

It has reference receiver mode, void return mode/logical type, a direct receiver,
and ordinary explicit argument modes. No other callable carries an initializer.

## Link inventory

Callable and class-initializer signatures use one exact form:

```text
c:<return_mode>:<T>(<kind>:<passing>:<T>,...);receiver:<receiver_mode>
```

`T` is `_C4` plus canonical type identity hex. Logical return type is retained,
including for indirect results. Definitions must exactly match their owned ABI.
Descriptor and static signatures remain `descriptor:file_class` and `global:<T>`.
Object artifacts retain the conservative `cloth_rt_alloc` requirement
`c:ptr(ptr)` and add `cloth_rt_array_alloc` as `c:ptr(i32,ptr)`.

## Validation and limits

Local validation checks exact shapes, arithmetic, alignment, maps, field order,
callable modes, and ownership. Full dependency-closure validation compares all
dependency-owned claims to their owners and reconstructs layouts before semantic
registration or native linking. Inline fields and class bases form the layout
graph; managed references do not. Cycles and missing owners are rejected.

Limits are 65,536 fields per struct, inline depth 128, padded value size 1 MiB,
65,536 references per value/layout, and 1,048,576 flattened map entries per
closure. Count each canonical struct value map and class heap map once.
The backend additionally caps aggregate backing storage plus aggregate
root-address slots at 256 KiB per callable.

Aggregate resource-policy violations produce `ArtifactIssueCode::kLimitExceeded`,
distinct from malformed metadata or invalid semantic models. Value and heap-descriptor
map counts are checked before constructing their decoded offset vectors.
Imported-model verification preserves this classification through artifact
validation; it is not inferred from diagnostic wording.

The 64-byte envelope, metadata limit 64 MiB, payload limit 1 GiB, JSON depth
limit 128, canonical decimal-string integers, and lowercase identity hex are
unchanged. `aggregate_tests.cc` freezes complete interface-artifact sizes and
SHA-256 identities for both targets, checks deterministic read/write round trips,
and rejects malformed maps, layouts, signatures, and dependency claims.
