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
- complete ordered enum cases with canonical owners, names, tags, locations,
  and public access independent of capitalization;
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

Override verification derives targets from owned class/interface declarations,
not from supplied dispatch tables. Local implementations require the existing
`is_override` marker. Their optional `overridden_identity` identifies only the
nearest replaced class function; it is absent for interface-only overrides.
Inherited methods are not required to anticipate interfaces later adopted by a
descendant. Unknown dependency-owned targets are deferred to closure verification
before declarations enter analysis or linking. Missing/spurious markers, final
replacement, and forged replaced-class identities are rejected without adding
artifact fields or changing physical calling conventions.

Static values use `ImportedScalarConstant`: a scalar kind and canonical bits,
interpreted through the member's canonical type identity. Extraction consumes
verified MIR constant data, never a returned initializer literal. Restoration
preserves exact IEEE bits and signed integer widths without source reparsing.

Extraction runs the verifier before returning a view. A failed extraction has
no partial view and reports deterministic record-scoped issues. Tests cover
private metadata, constant values, nullable signatures, unrelated-type
exclusion, inheritance, interfaces, constructor initialization, relocation and
source-order independence, lifetime independence, and representative corrupted
relationships.

Enums have scalar type layouts and empty shared class-layout records, never
heap reference maps. Enum-valued static constants retain their nominal type
and tag. Verification rejects empty/oversized or duplicate case sets, keyword
names, wrong owners, noncontiguous tags, numeric/reference confusion, and
invalid constants. External enum constant tags are checked against the loaded
dependency declaration before consumer analysis.

The version-4 `.cpa` reader/writer consumes and reconstructs this view. Its exact
record schema, bounded canonical encoding, integrity checks, compatibility gate,
and malformed-input policy are documented in
[artifact schema v5](artifact_schema_v5.md). Stage 23.3 connects artifact
dependency closure and direct-alias import visibility without reopening
dependency source files.


## Aggregate boundaries

Struct declarations retain their complete private field layouts, value reference
maps, and physical callable modes. They carry no heap descriptor or separate
constructor initializer. Local verification checks map shape, exact field order,
layout arithmetic, callable ownership/modes, and resource limits.

`verify_imported_package_closure` compares dependency-owned type claims against
their owning declarations and reconstructs inline layouts across the verified
closure. Cycles, missing owners, forged maps, conflicting claims, and aggregate
limits are checked before declarations enter semantic analysis or a native link.
The reconstruction follows only inline fields and class bases; managed references
do not create layout cycles.

Imported verification distinguishes malformed models from resource-limit
violations using `ImportedPackageIssueCode`. Artifact readers/writers preserve
limit failures as `ArtifactIssueCode::kLimitExceeded`; clients need not classify
diagnostic text. Reference-map counts are checked before constructing decoded
value/descriptor maps, and flattened-map expansion stops at its declared bound.
