# Cloth package artifact schema, version 8

This is the current `.cpa` contract. It inherits the frozen
[version-7 schema](artifact_schema_v7.md). Canonical JSON, record ordering,
integrity, payload, resource-limit, target, dependency, declaration, nullable,
and typed-error rules are unchanged except for the object and value-box
metadata described here.

## Compatibility

Header offset 8 contains little-endian format integer **8**. Compiler ABI **7**
uses `_C7` native names. Runtime ABI **12** includes the standard-error write
operation; runtime ABI 11 introduced managed value boxes. Compiler
capabilities advertise `artifact_formats: [8]`; receipts carry
`artifact_format: 8`. Process protocol **2**, receipt schema **1**, manifest
schema **1**, and toolchain-metadata schema **1** remain unchanged. The exact
compiler-paired standard-library package is `cloth` **v0.6.0**.

Formats 1–7 and compiler/runtime ABI mismatches are rejected and rebuilt. They
are never migrated or reinterpreted.

## Canonical Object identity

The `object` type record may carry the nominal identity of the exact
`cloth.lang.Object` declaration. The declaration remains a class record; the
distinct `object` semantic kind prevents a second root identity. Readers reject
an Object identity with another package, version, source package, name, or
nominal kind.

## Descriptor value-box fields

Every descriptor record adds these canonical keys:

- `boxed_value_type`: the exact canonical payload type identity, or `null`;
- `boxed_value_offset`: the object-relative payload byte offset, or zero;
- `value_box_virtuals`: whether the compiler-owned value-box virtual table is
  used.

The new descriptor kind is `value_box`. Enum and struct declarations receive a
value-box descriptor when the canonical Object root is in their compilation
closure. Its parent is `cloth.lang.Object`, its payload type is the owning
nominal type, and its precise reference map is the inline payload map shifted by
`boxed_value_offset`. Its external symbol signature is
`descriptor:value_box`.

Concrete primitive-wrapper classes remain `file_class` descriptors. Their
`boxed_value_type` and offset identify the one immutable payload field, while
their ordinary three-slot virtual table implements `Equals`, `HashCode`, and
`ToString`. Other file-class and error descriptors use null payload metadata,
zero offset, and `value_box_virtuals: false`.

Readers reconstruct layouts and reject missing Object ancestry, non-exact
payload identities, misaligned or out-of-bounds payloads, shifted-reference-map
mismatches, source virtuals on compiler-owned boxes, generic virtuals on an
ordinary class, and wrapper metadata that does not identify a declared field.

## Verified IR and runtime requirement

Boxing and unboxing are explicit verified MIR operations. Boxing copies an exact
primitive, enum, or struct payload to its canonical descriptor. A present
nullable value boxes its payload; absence becomes null. Checked unboxing tests
the exact descriptor and copies into the nullable result only on success.

Runtime ABI 11 owns allocation, exact descriptor checks, payload equality,
64-bit hashing, string conversion, and precise tracing for value boxes.

## Fixed fixture

`tests/unit/package_artifact_tests.cc` freezes the canonical format-8 interface
artifact:

- metadata length: `12454` bytes;
- metadata SHA-256:
  `18e1561d605622ad5d4fe2e17cc6b73dfff23e2a65170493e662dd2bafaa61e5`;
- complete artifact digest:
  `197e00f354e22f6c22d955b5aafd8916e0b9f56dc0c6f28a5bde623783d4feb6`.

Changing a type record, descriptor, layout formula, reference map, signature,
exact key, identity, ordering rule, or compatibility value requires an explicit
artifact-format review.
