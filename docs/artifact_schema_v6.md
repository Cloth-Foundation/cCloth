# Cloth package artifact schema, version 6

This is the current `.cpa` contract. It inherits the frozen
[version-5 schema](artifact_schema_v5.md). Record keys, canonical JSON,
ordering, integrity, payload, resource-limit, target, dependency, and typed-
error rules are unchanged.

## Compatibility

Header offset 8 contains little-endian format integer **6**. Compiler ABI **5**
and its `_C5` native names are unchanged. Runtime ABI **7** adds checked
Unicode-scalar string indexing and cursor traversal. Capabilities advertise
`artifact_formats: [6]`; receipts carry `artifact_format: 6`. Process protocol
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
indexing and iteration are compiler/runtime operations and therefore advance
only the runtime ABI, not this artifact format.

## Fixed fixture

`tests/unit/package_artifact_tests.cc` freezes the canonical format-6 interface
artifact:

- metadata length: `12377` bytes;
- metadata SHA-256:
  `ae8ac4df555229616170fc0d5e240bc9c2f7d7664ac826f696cffb4fbb0d064b`;
- complete artifact digest:
  `690cb7f59f21281562e70e0cc8b9ec60f15897366000fb2c31cfc7fc1dd433fa`.

Changing character validity, an exact key, identity, ordering rule, signature,
or compatibility value requires an explicit artifact-format review.
