# Cloth package artifact schema, version 1

This document freezes the implemented Stage 23.2 `.cpa` byte and metadata
schema. The design rationale and compatibility policy remain in the approved
[artifact proposal](proposals/stage_23_artifacts.md). This file owns exact field
names, enum spellings, and record shapes used by the reader and writer.

## Envelope

The artifact is one byte sequence: a 64-byte header, canonical JSON metadata,
and an optional native object payload.

| Offset | Width | Version-1 value |
| --- | --- | --- |
| 0 | 8 | `43 4c 54 48 50 4b 47 00` |
| 8 | 4 | little-endian format version `1` |
| 12 | 4 | little-endian flags `0` |
| 16 | 8 | little-endian metadata byte count |
| 24 | 8 | little-endian payload byte count |
| 32 | 32 | SHA-256 of the complete artifact with this field zeroed |

Metadata is limited to 64 MiB, payload to 1 GiB, and JSON nesting to 128.
Declared section sizes must exactly consume the input; truncation and trailing
bytes are errors. `interface` requires no native configuration and zero payload
bytes. `object` requires native configuration and one nonempty relocatable
object whose format and machine match the compatibility record.

## Canonical JSON rules

Metadata is UTF-8 without a BOM, whitespace, or terminal newline. Object keys
are sorted by UTF-8 bytes. Unknown, missing, duplicate, or out-of-order keys are
errors. Every integer is a canonical decimal JSON string. Boolean and null are
JSON literals; JSON numbers are forbidden.

Strings escape only quote, backslash, and control bytes. A control byte uses
lowercase `\u00xx`; alternative JSON escapes and escaped printable Unicode are
noncanonical. Invalid, overlong, or surrogate UTF-8 is rejected.

SHA-256 values are 64 lowercase hexadecimal characters. Binary canonical type,
nominal, and member identities are variable-length lowercase hexadecimal
strings. Link names remain their `_C2` spelling. Package and source names remain
ordinary UTF-8 strings.

The top-level object has exactly these keys:

```text
compatibility declarations dependencies kind layouts package sources symbols types
```

`kind` is `interface` or `object`. `package` has exactly `name` and `version`.

## Compatibility and inventory records

`compatibility` has:

```text
compiler_abi compiler_id native runtime_abi target
```

`compiler_abi` is `2` and `runtime_abi` is `1`. `compiler_id` is the selected
compiler executable digest. `target` has:

```text
endianness float64_alignment int64_alignment llvm_data_layout
object_header_words pointer_alignment pointer_size target_name
```

`endianness` is `little` or `big`. An interface artifact writes `native: null`.
An object artifact writes:

```text
code_model cpu features object_format relocation_model runtime_digest
target_triple tools
```

`features` is a sorted unique string array. `tools` is sorted by `name`; every
entry has `digest` and `name`. Version 1 recognizes x86-64 `coff`, `elf`, and
`mach_o` relocatable headers and checks their machine, endianness where
applicable, and selected triple family before accepting native bytes.

`sources` is sorted by `path`; each entry has `digest` and `path`. Paths are
normalized package-relative `/` paths ending in `.co` and exactly cover owned
file declarations. `dependencies` is sorted by `alias`; each entry has:

```text
alias digest name version
```

`symbols` is sorted uniquely by `link_name`; each entry has:

```text
abi_signature identity kind link_name role
```

`identity` is null only for runtime symbols. `role` is `definition` or
`requirement`. `kind` is `callable`, `constructor_initializer`, `static_field`,
`descriptor`, or `runtime`. Runtime symbols are requirements. Owned definitions
must exactly cover externally linked, non-abstract package ABI definitions and
must match their canonical link names.

## Type records

`types` is sorted uniquely by binary `id`. Every record has:

```text
abi_kind bit_width display_name element id kind nominal storage
```

`kind` uses the canonical `TypeKind` spelling. `abi_kind` is `invalid`, `void`,
`integer`, `float`, or `reference`. `storage` has decimal-string `alignment` and
`size`. `element` is an identity for array/nullable types and null otherwise.
`nominal` is null except for class/interface types, where it has:

```text
kind name package source_package
```

The nested package uses `name` and `version`. Readers reconstruct every type
identity and target representation rather than trusting the encoded `id`.

## Declaration records

`declarations` combines file and member records in unique binary `id` order.
Every record has a `record` discriminator.

A `file` record has:

```text
abstract abstract_functions base conformance direct_interfaces id
interface_functions interface_id interfaces kind location member_order name
record sealed source_package virtual_functions visibility
```

`kind` is `class` or `interface`; `visibility` is `public` or `private`.
Identity arrays contain binary identity hex. `conformance` is sorted by
interface identity; each entry has `functions` and `interface`. `location` has
decimal-string `column` and `line` plus package-relative `path`.

A `member` record has:

```text
abstract base_constructor final id kind location name override overridden
owner parameters record slot static static_value type visibility
```

`kind` is `field`, `function`, or `constructor`. Each parameter has `final`,
`name`, and `type`. Nullable wrappers remain in semantic type references while
member identity verification recursively applies overload erasure.

`static_value` is null except for supported static scalar constants. Its object
has `kind` and `value`:

| Kind | Value |
| --- | --- |
| `boolean` | JSON boolean |
| `character` | canonical decimal string |
| `integer` | canonical nonnegative decimal string checked against its declared type |
| `float32` | eight lowercase hexadecimal IEEE-754 bits |
| `float64` | sixteen lowercase hexadecimal IEEE-754 bits |

Non-finite floating constants and type/kind mismatches are rejected.

## Layout records

`layouts` has exactly one record per file, sorted by binary `owner`:

```text
alignment callables descriptor fields header_size owner size static_fields
```

Each field has `field`, `offset`, and `type`. A descriptor has:

```text
alignment display_name id interfaces kind mangled_name parent
reference_offsets size virtual_functions
```

Descriptor `kind` is `file_class`. Each interface dispatch has `functions`,
decimal-string `id`, and `interface`. Static-field ABI entries have `linkage`,
`mangled_name`, `member`, and `type`.

Each callable has:

```text
calling_convention initializer kind linkage mangled_name member parameters
return_type
```

The calling convention is `c`; linkage is `external` or `internal`. Parameters
have `kind` (`receiver` or `explicit`) and `type`. A function has
`initializer: null`. A constructor initializer has:

```text
id linkage mangled_name parameters return_type
```

The reader verifies layout bounds/alignment, inherited fields, managed-reference
offsets, descriptor ancestry, virtual slots, interface IDs and tables, callable
types and receiver placement, and allocation/initialization ownership before
returning an `ImportedPackageView`.

## Fixed fixture

`tests/unit/package_artifact_tests.cc` freezes a canonical interface artifact
containing `float32` and `int64` static constants plus a static `Main`:

- metadata length: `12112` bytes;
- metadata SHA-256:
  `5921bce3bef9122c57381570ba663a9f949897372c07c7636dfbc5ee319a3d75`;
- complete artifact digest:
  `3afbfd5f1da151e3126c2f8d3c2bfada3365348e5d2225e67e8e47d4fd4b8bd3`.

Changing a version-1 key, enum spelling, ordering rule, literal encoding, or
record shape changes this fixture and requires an explicit artifact-format
review rather than an incidental test update.
