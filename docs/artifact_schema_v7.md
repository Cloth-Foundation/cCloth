# Cloth package artifact schema, version 7

This is the current `.cpa` contract. It inherits the frozen
[version-6 schema](artifact_schema_v6.md). Canonical JSON, record ordering,
integrity, payload, resource-limit, target, dependency, declaration, and
typed-error rules are unchanged except where this document extends nullable
types.

## Compatibility

Header offset 8 contains little-endian format integer **7**. Compiler ABI **6**
uses `_C6` native names. Runtime ABI **10** adds bounded regular-file byte input
and retains the ABI-9 tagged nullable-value assertion guard. Capabilities
advertise `artifact_formats: [7]`; receipts carry
`artifact_format: 7`. Process protocol **2**, receipt schema **1**, manifest
schema **1**, toolchain-metadata schema **1**, and the compiler-paired `cloth`
package version **0.4.0** is compiler-paired.

Formats 1–6 and compiler/runtime ABI mismatches are rejected and rebuilt. They
are never migrated or reinterpreted.

## Nullable type records

A nullable record retains `kind: "nullable"` and the canonical `element`
identity. If the element's ABI kind is `reference`, the nullable record has the
same pointer storage and `[0]` reference map as the element.

Primitive, enum, and struct elements use `abi_kind: "aggregate"` and bit width
zero. Let `T` have size `S`, alignment `A`, and reference offsets `R`. The
canonical wrapper is:

```text
tag offset     = 0
payload offset = align_up(1, A)
alignment      = A
size           = align_up(payload offset + S, A)
references     = each offset in R plus payload offset
```

The one-byte tag is zero for absent and one for present. An absent wrapper has
a zeroed payload. Other tag values are invalid compiler/runtime state. Readers
reconstruct the layout from the element and target, then reject mismatched ABI
kind, bit width, size, alignment, reference offsets, identity, or element kind.
The existing aggregate size, depth, reference-map, and callable-storage limits
remain in force.

## Callable ABI

Nullable value parameters use `value_pointer`; nullable value returns use an
indirect `result_pointer`. This is the same compiler-owned aggregate convention
used by structs and does not expose a platform C aggregate ABI. Nullable
references remain direct pointer parameters and returns. Callable verification
reconstructs these modes from the canonical type layout.

## Runtime requirement

Object artifacts record `cloth_rt_require_nullable_value` as a runtime symbol
requirement with signature `c:void(i8)`. Runtime ABI 9 validates the presence
tag for postfix non-null assertion without interpreting the payload bytes.

## Fixed fixture

`tests/unit/package_artifact_tests.cc` freezes the canonical format-7 interface
artifact:

- metadata length: `12377` bytes;
- metadata SHA-256:
  `e1f96c626adaa064b0fe1b53a26cae939481d4254514a9b7630d7ca5064e37ce`;
- complete artifact digest:
  `87e5e90e95f5ee097f89d148b9aa1d46bf9be4045fb49fdfff9cfa5324582fcc`.

Changing a type record, layout formula, reference map, signature, exact key,
identity, ordering rule, or compatibility value requires an explicit artifact
format review.
