# Cloth package artifact schema, version 5

This is the current `.cpa` contract. It inherits the frozen
[version-4 schema](artifact_schema_v4.md), except for the typed-error records
and compatibility transition below. Canonical JSON, exact-key validation,
ordering, integrity, payload, resource-limit, target, and dependency rules are
unchanged.

## Compatibility

Header offset 8 contains little-endian format integer **5**. Compiler ABI **5**
uses `_C5` native names. Runtime ABI **5** includes the compiler-known error
descriptors, terminal reporter, and owned program-argument operation.
Capabilities advertise
`artifact_formats: [5]`; receipts carry `artifact_format: 5`. Process protocol
**2**, receipt schema **1**, and manifest schema **1** are unchanged. Formats
1–4 and older compiler/runtime ABIs are rejected and rebuilt, never migrated.

## Error types and declarations

Type records retain their version-3 keys. Semantic `kind` additionally admits
`"error class"`. A user error has a class nominal identity, null `element`,
reference ABI/storage, and the ordinary single managed-reference map. The
compiler-known `Error` and `DivisionByZero` identities have no package nominal
record and are accepted only under their exact compiler-owned identities.

File records retain their keys and additionally admit `kind: "error"`. A direct
error has null `base`; a derived error names its canonical error-file base.
Error fields, constructors, functions, interfaces, abstract/final state,
visibility, member order, and virtual slots use the class rules. Validation
rejects ordinary-class/error ancestry mixing and non-error bases.

Every member record adds the exact key `throws`:

```text
abstract base_constructor final id kind location name override overridden
owner parameters record slot static static_value throws type visibility
```

`throws` is a canonical-identity array. It is empty for fields and nonthrowing
callables. Callable sets are sorted, unique, contain only error types, and
preserve public contracts for source-free semantic checking. Private inferred
effects do not enter unrelated public records.

## Error descriptors

Every descriptor adds `parent_is_error_root` and admits `kind: "error"`:

```text
alignment display_name id interfaces kind mangled_name parent
parent_is_error_root reference_offsets size virtual_functions
```

A direct user error has null `parent` and `parent_is_error_root: true`; a
derived error names its canonical base-file identity in `parent` and sets the
flag false. Ordinary descriptors set the flag false. Error descriptor symbols
use `descriptor:error`; ordinary managed classes retain
`descriptor:file_class`.

## Callable error ABI

Every callable record adds the Boolean key `error_abi`. A nonthrowing callable
sets it false and retains its version-4 physical ABI. A throwing callable sets
it true and physically returns a nullable managed `Error` reference. Its logical
`return_type` remains unchanged. A non-void success result uses
`return_mode: "indirect"` and a leading `result`/`result_pointer` parameter;
throwing `void` has no result parameter. Null error means the result was
initialized, while non-null error means it was not written.

Constructor initializers use the same error channel as their constructor.
Virtual and interface slots retain `error_abi: true` when their contract may
throw, including a nonthrowing implementation of a throwing declaration.
Closure validation rejects declaration, slot, initializer, and physical-
signature disagreement. Stage 37 reuses these callable records for an exact
non-null `string[]` entry parameter; no artifact key or format revision is
needed.

Link-inventory callable signatures are now prefixed with either `c:plain:` or
`c:error:` before the existing return mode, logical type, parameters, and
receiver mode. This prevents a throwing definition from satisfying a
nonthrowing requirement with the same source signature. Runtime ABI 5 retains
`DivisionByZero` construction and uncaught-error reporting and adds the native
program-argument operation; Shuttle transports artifact metadata and object
payloads without interpreting those operations.

## Determinism and validation

Readers verify canonical throws order and coverage types, error ancestry,
descriptor shape, ABI mode/parameters, symbol signatures, and complete
dependency ownership before semantic registration or linking. Source-free
consumers reconstruct the same error hierarchy and callable effects as a
whole-project compilation. Malformed format-5 records, format-4 inputs, and
mixed ABI closures are rejected before output replacement.

## Fixed fixture

`tests/unit/package_artifact_tests.cc` freezes the canonical format-5 interface
artifact:

- metadata length: `12377` bytes;
- metadata SHA-256:
  `df1f1a9fb9db3e3663b60867f821f132bc3da9ef69d7f17e1d9df808eb4efc75`;
- complete artifact digest:
  `85841ba251c75eea3585d52967b831e0139aa5607d7beb69a12e5cadfee7bd40`.

Changing an exact key, identity, ordering rule, signature prefix, or error
encoding requires an explicit artifact-format review rather than an incidental
golden update. Runtime-only compatibility transitions retain the record shape
but intentionally update the compatibility value and complete-artifact digest.
