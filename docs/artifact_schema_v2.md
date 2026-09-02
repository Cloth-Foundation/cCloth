# Cloth package artifact schema, version 2

This is the current `.cpa` contract. It inherits every envelope, canonical JSON,
record, validation, and resource-limit rule in the frozen
[version-1 schema](artifact_schema_v1.md), except for the explicit changes below.
No other keys or encodings change. Version-1 artifacts are rejected, not
upgraded or interpreted as version 2.

## Compatibility

The 32-bit little-endian format field at header offset 8 is `2`.
`compatibility.compiler_abi` is the decimal string `"3"`; `runtime_abi` remains
`"1"`. Native Cloth names use `_C3` followed by the existing canonical identity
encoding. Exact compiler, target, dependency, runtime, and native-tool identity
checks remain in force.

Shuttle's capabilities and artifact receipts advertise artifact format 2 as
an ordinary JSON integer. Process protocol 2, receipt schema 1, and manifest
schema 1 are unchanged. Shuttle still treats artifact contents as opaque.

## Enum type and case records

Type `kind` and nominal `kind` admit `enum`. An enum type has a nominal
identity, no `element`, `abi_kind: "integer"`, `bit_width: "32"`, and the target's
`uint32` storage size/alignment. An enum cannot be a nullable element, but an
array of enums can itself be nullable. Class/interface encodings are unchanged.

Every `file` declaration now has the following exact sorted keys:

```text
abstract abstract_functions base conformance direct_interfaces enum_cases id
interface_functions interface_id interfaces kind location member_order name
record sealed source_package virtual_functions visibility
```

`kind` also admits `enum`. `enum_cases` is empty for classes/interfaces and
contains 1 to 65,536 records for enums, in tag order. Each case has exactly:

```text
id location name tag
```

`id` is canonical binary identity encoded as lowercase hex: member domain
`enum-case`, nominal enum owner, exact case name, and an empty parameter list.
`location` uses the unchanged `column`, `line`, and `path` record. `name` must
be a valid non-keyword identifier and unique within its owner; comparison is
case-sensitive. `tag` is a canonical nonnegative decimal string equal to its
zero-based position. Tags are internal representation, not stable wire IDs.

Cases are public by construction. There is no case `visibility` key; an added
key, including a private-case marker, is rejected by exact-schema validation.
The file's visibility still follows its stem. Case records do not appear in
the ordinary member declaration table or `member_order`.

An enum file has no abstract/sealed flags, base, interface ID, member order,
functions, virtual slots, interfaces, conformance, or ordinary member records.
Its type name and complete ordered cases survive source-free imports.

## Enum constants and layout

The static scalar literal table additionally admits:

| Kind | Value |
| --- | --- |
| `enum` | canonical decimal tag string, with the field's nominal enum type |

Type/kind mismatches and tags outside the declared owner's case set are errors.
For dependency-owned enum types, the decoder checks scalar type and tag bounds;
dependency loading checks the exact tag against the verified enum declaration
before the imported member becomes usable. Other literal encodings are unchanged.

An enum's shared file-layout record has `header_size: "0"`, `size: "0"`,
`alignment: "1"`, and empty `fields`, `callables`, and `static_fields`. Its
descriptor placeholder retains canonical `descriptor` identity and nominal
display name, `kind: "file_class"`, `size: "0"`, `alignment: "1"`, empty
`mangled_name`, null `parent`, and empty reference, virtual, and interface tables.
This placeholder is not a runtime heap descriptor; the type record alone owns
the four-byte scalar layout. No descriptor definition or case symbol appears
in the link inventory. Enum print tables/helpers are consuming-module-private.

## Fixed fixture

`tests/unit/package_artifact_tests.cc` fixes the version-2 encoding of the
existing interface fixture with `float32`/`int64` static constants and `Main`:

- metadata length: `12128` bytes;
- metadata SHA-256:
  `78b12fcaeda3b73c64528ab9957d87fcdccdc606bab6915cffec247dd049e554`;
- complete artifact digest:
  `f3904b9738d4fe9023c5fd7cbc39907cbd66cdd8a81f31ace0a57a3ca0ed329c`.

The enum suite separately round-trips all case spellings and rejects malformed
case identities, names, tags, layouts, and constants. Cross-tool tests verify
source-free consumption and byte-identical serial/parallel package artifacts.
Any further record-shape change requires an explicit format review.
