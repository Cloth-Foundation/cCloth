# Cloth package artifact schema, version 4

This is the current `.cpa` contract. It inherits the frozen
[version-3 schema](artifact_schema_v3.md), except for the envelope version and
the integer constant rule below. Record keys, ordering, canonical identities,
layouts, symbol inventories, integrity, payloads, and dependency rules do not
change.

## Compatibility

Header offset 8 contains little-endian format integer **4**. Compiler ABI **4**
(`_C4` names), runtime ABI **2**, process protocol **2**, receipt schema **1**, and
manifest schema **1** are unchanged. Earlier artifacts must be rebuilt; the
reader does not migrate or reinterpret them.

Capabilities advertise `artifact_formats: [4]`; receipts carry
`artifact_format: 4`. Shuttle requires these values but continues to treat the
artifact as opaque. Exact compiler identity, target, native-tool compatibility,
and dependency digests remain mandatory, even when a changed dependency produces
the same constant value.

## Static scalar values

A static field's declared canonical type identity determines the interpretation
of its `static_value`. Its shape remains exactly `kind` and `value`.

For `kind: "integer"`, `value` is a string containing the mathematical decimal
value. Signed integers admit their full negative and positive ranges; unsigned
integers and `byte` admit only nonnegative values in their declared range.
Reject leading `+`, leading zeros except `"0"`, `"-0"`, whitespace, fractions,
and out-of-range values.

```json
{"kind":"integer","value":"-128"}
{"kind":"integer","value":"-9223372036854775808"}
{"kind":"integer","value":"18446744073709551615"}
```

These examples require `int8`, `int64`, and `uint64` respectively. All other
scalar encodings are unchanged:

- Boolean values are JSON booleans.
- Character values are canonical decimal strings in `0..255`.
- `float32` and `float64` values are exactly 8 or 16 lowercase hexadecimal
  digits containing finite IEEE bits. Signed zero and subnormals are preserved.
- Enum values are canonical decimal tags validated against their exact owning
  enum declaration, including dependency-owned declarations.

The reader restores typed scalar bits directly. It never reparses float bits as
decimal source text or reevaluates unavailable initializer expressions. HIR/MIR
and imported-model verifiers independently reject missing, ill-typed, invalid,
or inconsistent constant claims. Static fields emit constant native globals,
not executable initializer bodies, heap objects, or GC roots.

Reference-valued and aggregate constants, mutable static storage, and dynamic
initialization remain unsupported. The source evaluation and resource rules are
owned by the [scalar constant contract](proposals/stage_28_scalar_constants.md).
