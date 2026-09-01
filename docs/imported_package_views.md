# Imported package declaration and ABI views

Stage 23.2 introduces an owned compiler boundary between verified package
compilation and artifact serialization. `build_imported_package_view` extracts
one exact manifest package from a verified semantic/MIR/ABI graph and returns a
detached `ImportedPackageView`. The result owns every string and collection; it
remains valid after the source snapshots, syntax, semantic model, HIR, MIR, and
ABI module are destroyed.

## Record boundary

The view retains the information a source-free consumer needs:

- exact package, nominal, structural type, and member identities;
- capitalization-derived visibility, modifiers, declaration order, parameter
  types, base-constructor selection, virtual slots, overrides, abstract
  contracts, interfaces, and conformance maps;
- package-relative declaration locations and typed static scalar literals;
- target type and class layouts, including inherited fields, reference offsets,
  descriptor ancestry, virtual tables, and interface dispatch tables; and
- callable, static-storage, descriptor, allocation, and constructor-initializer
  ABI identities, linkage, calling convention, receiver placement, and types.

File and member records are emitted only for the requested owner. The type table
is the recursive closure actually referenced by that package, so it may contain
dependency type identities but cannot absorb unrelated declarations from the
temporary whole-project graph. Private declarations remain present with private
visibility for layout and access diagnostics; the view does not promote them.

Cross-record relationships use canonical binary identities. `FileId`, `TypeId`,
`SymbolId`, absolute paths, dependency aliases, source-backed `string_view`
values, and executable bodies do not cross the boundary. No AST, HIR, MIR,
local-variable record, or dependency implementation body is retained.

## Verification

`verify_imported_package_view` treats its input as untrusted structured data.
It checks package ownership, canonical ordering and uniqueness, exact identity
reconstruction (including nullable overload erasure), source locations,
visibility, structural type closure, target type representations, declaration
order, layout bounds and alignment, GC reference offsets, descriptor ancestry,
interface IDs and dispatch, static storage, callable signatures, receiver
placement, and constructor initializer ownership.

Extraction runs the verifier before returning a view. A failed extraction has
no partial view and reports deterministic record-scoped issues. Tests cover
private metadata, constant values, nullable signatures, unrelated-type
exclusion, inheritance, interfaces, constructor initialization, relocation and
source-order independence, lifetime independence, and representative corrupted
relationships.

The version-1 `.cpa` reader/writer consumes and reconstructs this view. Its exact
record schema, bounded canonical encoding, integrity checks, compatibility gate,
and malformed-input policy are documented in
[artifact schema v1](artifact_schema_v1.md). Artifact dependency closure and
import visibility are connected in Stage 23.3.
