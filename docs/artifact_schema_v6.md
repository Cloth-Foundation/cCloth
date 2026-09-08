# Cloth package artifact schema, version 6

This is a frozen predecessor of the current
[version-7 schema](artifact_schema_v7.md). It inherits the frozen
[version-5 schema](artifact_schema_v5.md). Record keys, canonical JSON,
ordering, integrity, payload, resource-limit, target, dependency, and typed-
error rules are unchanged.

## Compatibility

Header offset 8 contains little-endian format integer **6**. Compiler ABI **5**
and its `_C5` native names are unchanged. Runtime ABI **8** adds allocating,
checked Unicode-scalar string slicing after ABI 7 traversal. Capabilities
advertise `artifact_formats: [6]`; receipts carry `artifact_format: 6`. Process protocol
**2**, receipt schema **1**, manifest schema **1**, and toolchain-metadata schema
**1** are unchanged. Formats 1–5 are rejected and rebuilt rather than migrated
or reinterpreted.

## Unicode character constants

A `static_value` record with `kind: "character"` carries a canonical unsigned
decimal string in `value`. The decoded integer must be a Unicode scalar:
`0..1114111`, excluding the surrogate interval `55296..57343`. Leading signs,
leading zeroes except for `"0"`, whitespace, non-decimal spelling, surrogates,
and values above U+10FFFF are invalid.

This widens the version-5 byte-only range without changing `char` storage.
`char` remains an unsigned four-byte, four-aligned compiler value and lowers to
LLVM `i32`. Readers validate the scalar before semantic registration; writers
reject invalid compiler state. Package imports and source-free consumers retain
the exact scalar bits.

String contents remain source/runtime values and do not add artifact records.
Unicode escapes are decoded before lowering to canonical UTF-8. String
indexing, iteration, and slicing are compiler/runtime operations and therefore
advance only the runtime ABI, not this artifact format.

## Fixed fixture

The Stage 40 checkpoint froze this canonical format-6 interface artifact:

- metadata length: `12377` bytes;
- metadata SHA-256:
  `944bb140f890e8e69b2110f748c42d00840168a9bdc95d771fd33a3935be6b27`;
- complete artifact digest:
  `c18335405925b98550836031e3c38bd99f0d634cdfe094f2a7a261d0c58cb618`.

Changing character validity, an exact key, identity, ordering rule, signature,
or compatibility value requires an explicit artifact-format review.
