# Proposal: Stage 23 package artifacts

Status: **approved Stage 23.1 contract; Stage 23.3 pipeline implemented
2026-09-01**.

This proposal defines the compiler-owned boundary for separate compilation.
Its companion [process protocol](../../shuttle/docs/proposals/compiler_protocol_v2.md)
defines how Shuttle requests the work. Both contracts and the implementation
start were approved together. The work ledger records implemented portions;
approval alone does not claim that artifact or protocol operations exist.

## Objective and scope

Compile each manifest-defined local package independently. A consumer must be
able to resolve imports, check types, derive classes, implement interfaces, and
generate code using dependency artifacts without reading dependency sources.
Linking must preserve the behavior of the current whole-project pipeline.

The unit of compilation is one Shuttle package, not one `.co` file. All source
files within that package still use the existing two-pass registration and
definition analysis. Source import cycles within a package remain valid;
Shuttle package dependency cycles remain invalid.

There is no new source syntax or manifest version. Stage 23 does not add remote
dependencies, automatic incremental caching, shared libraries, LTO, debug
information, FFI, static initialization, or ABI stability across releases.

## Approved decisions

1. A `.cpa` file is a self-contained Cloth package artifact. It contains
   versioned semantic/ABI metadata and, for native builds, one relocatable
   object. It does not contain dependency artifacts or require sibling files.
2. An `interface` artifact contains verified declarations and ABI metadata but
   no native object. An `object` artifact adds native code. The former supports
   `shuttle check` without requiring `llc`, a native linker, or a runtime archive;
   it is not a linkable library. Here `interface` means a declaration artifact,
   not a package restricted to Cloth interface declarations.
3. Dependencies are pinned by package identity and exact artifact digest.
   Package versions, timestamps, and filenames alone never establish validity.
4. Persistent identities are logical, not process-local indices. Native
   descriptors and accessible base-constructor initializers have one owning
   definition, referenced by other packages.
5. `clothc` owns artifact parsing, verification, compatibility, and native
   linking. Shuttle consumes public receipts and supplies paths; it does not
   deserialize semantic or ABI records.
6. Protocol v1 and direct compiler use remain supported. Protocol v2 introduces
   separate compilation without changing v1's command or output meanings.

## Artifact envelope, version 1

One regular file contains this header, followed immediately by metadata bytes
and then payload bytes. Header integers are unsigned little-endian values;
there is no native-structure serialization, padding, compression, or archive
extraction.

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 8 | Magic bytes `43 4c 54 48 50 4b 47 00` (`CLTHPKG` plus NUL) |
| 8 | 4 | Artifact format version, initially `1` |
| 12 | 4 | Reserved flags, exactly zero in version 1 |
| 16 | 8 | Metadata byte count |
| 24 | 8 | Native payload byte count; zero for `interface` |
| 32 | 32 | Artifact SHA-256 digest |

The digest covers the entire file with bytes 32 through 63 treated as zero.
Its external spelling is 64 lowercase hexadecimal digits. Extra bytes,
overflowing sizes, truncation, unsupported versions, nonzero flags, and digest
mismatches are errors. Input sizes are checked before allocation. Version 1
limits metadata to 64 MiB, payload to 1 GiB, and structural nesting to 128;
exceeding a limit is a diagnostic, not a partial read or process failure.

Metadata is canonical UTF-8 JSON with no BOM or terminal newline. Object keys
are sorted by UTF-8 bytes, arrays use their specified order, duplicate or
unknown fields are rejected, and there is no insignificant whitespace. Writers
escape quote and backslash, encode U+0000 through U+001F as lowercase `\u00xx`,
and otherwise write valid UTF-8 directly. All integer-valued fields use decimal
strings without leading zeroes; raw JSON numbers are forbidden. Signed
integers use `-` only for negative values, never for zero. Booleans and null use
the ordinary JSON literals. Float constants store their IEEE bit patterns as
8 (`float32`) or 16 (`float64`) lowercase hexadecimal digits, without `0x`,
rather than decimal JSON numbers. Readers reject noncanonical
encodings, invalid UTF-8, and unpaired surrogate escapes.

This is an explicit serialized record model, not a JSON dump of C++ objects.
Field names and enum spellings belong to artifact version 1; C++ enum values,
`string_view`, addresses, `size_t`, `FileId`, `TypeId`, and `SymbolId` do not.
The implementation must provide schema fixtures and a bounded reader/writer
before connecting artifact inputs to semantic analysis.

## Required metadata

The top-level record has exactly `kind`, `package`, `compatibility`, `sources`,
`dependencies`, `types`, `declarations`, `layouts`, and `symbols` fields.
Collections and references carry the following contract:

| Record | Required information and ordering |
| --- | --- |
| `kind` | `interface` or `object`; must agree with payload presence |
| `package` | Manifest name and exact declared SemVer, including any build metadata |
| `compatibility` | Format-independent compiler, ABI, target, and native configuration described below |
| `sources` | Logical `/`-separated `.co` paths and SHA-256 of the exact bytes compiled, sorted by path |
| `dependencies` | Direct alias, target package name/version, and required artifact digest, sorted by alias |
| `types` | Canonical primitive, nominal, array, and nullable type references, sorted by canonical identity |
| `declarations` | Every owned file type and member declaration, sorted by canonical identity; declaration-order lists are explicit |
| `layouts` | Verified target layouts, descriptors, dispatch tables, and callable ABI contracts, sorted by owner identity |
| `symbols` | Defined and required nonlocal Cloth symbols and required runtime symbols, sorted by link name |

There are no absolute source/artifact paths, timestamps, import aliases embedded
in nominal identities, or selected executable entry in an artifact. The source
inventory identifies the exact snapshot used to compile, not permission to
reopen that source. The compiler reads each source into an immutable snapshot
and hashes the same bytes it parses.

### Semantic declarations

The declaration records preserve:

- file identity, source package and stem, class/interface kind, capitalization
  visibility, abstract/sealed status, direct base, and direct interfaces;
- member identity, owning type, source spelling, kind, visibility, declared
  type, ordered parameter types, and `final`, `static`, `override`, and
  `abstract` flags;
- constructor allocation/initialization signatures and base-constructor
  selection; private constructor spellings retain their current meaning;
- original member order, inherited virtual slots, overridden declarations,
  outstanding abstract contracts, flattened interface contract slots, and
  concrete conformance mappings;
- the typed literal values of currently supported constant static fields; and
- declaration locations as package-relative file path and one-based line and
  column, allowing diagnostics without source text or an original checkout.

Nominal type references use canonical identities, including for dependencies.
Array and nullable references are structural; aliases such as `int`, `uint`,
and `float` resolve to their existing canonical primitive types. Nullability
remains semantic metadata even where its ABI representation is erased.

Private declarations are retained for layout, inheritance validation, and
access diagnostics. Their presence in metadata does not make them importable
or callable. No dependency implementation bodies, local variables, AST, HIR,
or MIR are serialized. Each producer verifies its own bodies through the
existing boundaries; consumers build imported declaration views and never
reanalyze dependency bodies or synthesize fake source ASTs.

Loading transitive metadata does not grant transitive import visibility. Only
the compiling package's supplied direct aliases participate in imports. A type
appearing in an accessible signature retains its canonical identity and current
access rules even when its owning package is not directly importable. No
implicit alias or new re-export feature is introduced.

### ABI and native payload

Layouts include target sizes/alignments, complete padded base prefixes, owned
and inherited field identities/types/offsets, managed-reference offsets,
descriptor parent identities, and virtual/interface table contents and slots.
Callable contracts include calling convention, return and parameter ABI types,
receiver placement, linkage, and allocation/initialization entry identities.
Static storage remains separate from instance layout.

An `object` payload is exactly one relocatable native object for the declared
target, containing only this package's definitions. Imported functions, static
fields, and descriptors are references, not copies. It contains no native
`main`, bundled runtime, dependency objects, arbitrary linker options, or paths
to libraries. An interface-only package still produces a valid object payload
for an `object` request, even when it has no executable bodies.

The producer verifies MIR and ABI before emission and verifies the native
object's format/machine and nonlocal Cloth symbol inventory against the emitted
module. Generated platform support symbols are governed by the compiler's
native-toolchain contract, not by instructions read from artifact metadata.
Consumers validate semantic/ABI metadata and payload integrity before passing
code to native tools. Native-object parsing and linking failures remain explicit
toolchain diagnostics.

Metadata and checksums are not proof that arbitrary native code is trustworthy.
Artifacts are executable build inputs from the selected local toolchain; Stage
23 supplies neither signing nor a sandbox for hostile object code.

## Canonical identity and ownership

Package identity is the tuple of manifest name and exact version. A graph still
admits only one version of each package name. Nominal identity adds the ordered
source-package components, file stem, and class/interface kind. Import aliases,
absolute directories, enumeration order, and local compiler indices are absent.

Use domain-tagged, length-prefixed UTF-8 components for canonical identity
bytes: each component is its byte length as a little-endian `uint64` followed
by those bytes; lists begin with a little-endian `uint64` count. Primitive
types have fixed textual tags; arrays and nullable types wrap their element
identity. A callable adds its kind, source name, and ordered overload parameter
identities. Return type and modifiers are checked signature data, not overload
discriminators. Nullable wrappers are recursively erased from overload parameter
identity as in the current ABI. Static field and descriptor identities have
distinct domain tags. Allocation and initialization entries are distinct
callable kinds.

The ABI revision uses `_C2` followed by lowercase hexadecimal canonical symbol
identity bytes for external Cloth names. This deliberately favors an injective
encoding over truncated hashes. The complete tag/record fixtures are part of
23.2's identity tests and must follow this contract across every compiler path.
Direct compilation uses a distinct standalone-owner domain with no filesystem
path; direct-mode artifacts are not a Stage 23 input kind.

Source-visible qualified names and `::typeName` do not acquire package versions
or ABI encodings. Canonical link identity and display spelling are separate.

| Definition | Owner and linkage |
| --- | --- |
| Class descriptor | Defined once by its package under its canonical external symbol; other packages declare it |
| Public callable or constant static field | Defined by its declaring package; externally referenceable |
| Accessible constructor initialization entry | Defined by its declaring package; externally referenceable for derived construction, never a source member |
| Private callable, private constructor entries, field-initializer helper | Package-local implementation; no new source access |
| Descriptor tables and string literal bytes | Private backing storage in the descriptor/code owner's object |
| Primitive runtime descriptors and collector state | Defined once by the linked runtime |
| Native entry wrapper | Generated once by `clothc` during the final link operation |

Descriptor linkage is an ABI requirement even for private classes, not public
language visibility. Distinct nominal descriptors must not be merged merely
because their contents match. `is`/`as` tests keep comparing the same descriptor
address and following parent pointers. Base construction keeps operating on the
same most-derived allocation; it must not allocate or install a duplicate base
descriptor. This addresses the current module-local descriptor names and
unconditionally internal constructor-initializer linkage.

Interface IDs retain the existing FNV-1a 64-bit algorithm, now over canonical
nominal identity bytes: offset `14695981039346656037`, prime `1099511628211`,
unsigned arithmetic modulo 2^64. Metadata stores both full identity and ID.
Readers recompute the ID and diagnose collisions between different identities
over the complete supplied closure, including private interfaces. IDs are never
reassigned by load order. Interface tables remain sorted numerically by ID;
contract slot order is retained from the defining artifact.

No general runtime registration hook is introduced. Compiler-owned descriptors,
linked parent/table references, existing per-frame GC roots, and one runtime
instance suffice for the current object model. Reference-valued/dynamically
initialized static storage remains deferred. Cross-package tracing must include
both inherited and locally declared reference offsets.

## Compatibility and dependency closure

Compatibility requires exact equality, not inferred SemVer compatibility:

- artifact format `1`, compiler ABI revision `2`, and runtime ABI revision `1`
  for the unchanged descriptor/call/root-frame representation;
- `compiler_id`: SHA-256 of the selected `clothc` executable bytes;
- logical target and complete Cloth target-data-layout values; and
- for `object`, the resolved native triple, object format, runtime archive
  digest, native tool executable digests, and effective code-generation/link
  configuration, including fixed CPU/features and relocation/code model.

Interface artifacts record native configuration as null and do not probe native
tools. Object artifacts record the configured native environment, distinguishing
for example Windows MSVC, Windows GNU, Linux, and macOS rather than treating
all `x86_64` code as interchangeable. The selected compiler resolves its own
native environment; this does not add cross-compilation toolchain configuration
to the manifest. Compiler instrumentation builds therefore cannot silently
share artifacts with a different compiler binary.

Executable/tool paths are transport details, not identity. Identical tool
bytes/configuration relocated on disk remain compatible. Executable digests
are conservative compatibility gates, not a claim of ABI stability or a full
system-library fingerprint. Native builds assume a fixed selected toolchain
installation for one invocation; concurrent toolchain replacement is unsupported.
The artifact reader must never guess compatibility from product version `0.1.0`.

Each direct dependency pins its exact artifact digest. A request supplies the
complete reachable artifact closure explicitly. The compiler rejects missing
records, unexpected extra packages, cycles, duplicates, self-dependencies,
different versions of one name, and inconsistent digests reached through a
diamond. It checks every artifact's dependency edges, not just the root's.
Changing dependency code requires rebuilding its consumers even if its public
API is unchanged; selective API-based invalidation is deferred.

All artifacts in an interface compilation closure have kind `interface`; all
artifacts in an object compilation/link closure have kind `object`. There is
no implicit upgrade, object-to-interface substitution, or artifact conversion
in version 1. This keeps a digest pin tied to exactly the data verified.

This is artifact compatibility, not source freshness. A valid old artifact can
still represent old source. Shuttle compiles every package once per invocation
and reuses that result across consumers of the same graph node. It does not
skip compilation merely because an output file exists. Compiler protocol tests
can explicitly reuse compatible supplied artifacts without reopening sources;
automatic reuse between Shuttle commands remains a later caching stage.

## Linking and failure behavior

`clothc` validates the entire object closure before invoking the native linker:
identities, digests, target/ABI compatibility, unique owning definitions,
required Cloth symbols/signatures, descriptor ownership, layouts, interface
IDs/slots, and dependency closure. Runtime requirements must match the selected
compiler runtime. Source-private declarations are not promoted during linking.

The root artifact and logical entry path select exactly one eligible public
static `Main` using the existing signature rules. Package compilation never
creates an entry wrapper; dependency `Main` declarations are ordinary functions.
The linker operation generates one wrapper, supplies each package object once,
and links the runtime once. It never reparses sources or relowers dependency
MIR. Missing platform symbols/tool failures are reported with the failing native
stage, after compiler-owned checks have passed.

Deterministic validation orders packages by identity, declarations by canonical
identity, and symbols by link name. Artifact errors include the package, input
artifact path, and violated contract; dependency mismatches identify the owner
and expected/actual identity or digest. Inaccessible source declarations remain
source diagnostics at the consumer's use, with a logical declaration location
when useful. Readers must reject malformed indices/references, duplicate
identities, invalid flags, impossible layouts, malformed signatures, cyclic
ancestry, and inconsistent dispatch/GC metadata before constructing trusted
semantic or ABI objects.

Outputs use exclusive private staging beside the destination and atomic file
replacement after validation. Generation, validation, or publication failure
preserves previous artifacts/executables. A later process/receipt-transport
failure can leave a newly published output, but Shuttle must still stop rather
than consume it as a successful result.
Input artifacts are read from a stable opened file/snapshot; the bytes verified
are the bytes consumed, not a subsequently reopened path. A failed producer
does not cause Shuttle to use an older artifact left at its output path.

## Verification and stage handoff

- **23.1:** artifact and process contracts approved with implementation
  authorization on 2026-08-31.
- **23.2 (complete 2026-08-31):** canonical identity, external ownership,
  imported declaration/ABI views, and ABI-2 whole-project lowering are in place.
  The frozen v1 schema now has bounded deterministic read/write, integrity and
  compatibility verification, and malformed-input coverage.
- **23.3 (complete 2026-09-01):** independent package compilation,
  entry-wrapper linking, public receipts, deterministic Shuttle orchestration,
  and OS-backed writer exclusion are connected. No automatic cache.
- **23.4:** complete positive/negative equivalence and development/sanitizer
  audits before activating another stage.

Required tests cover metadata round trips and canonical byte determinism;
truncated, oversized, corrupted, duplicate, and incompatible artifacts; alias
and checkout relocation; missing/mixed-version diamond dependencies; wildcard
imports and private declarations; equal class stems in different packages;
cross-package construction, inherited fields, `super`, abstract/covariant/final
overrides, interfaces and injected ID collisions; casts and `::typeName`;
nullable references, arrays/strings, and cross-package GC stress; independent
compilation after dependency sources are unavailable; one entry/runtime; and
atomic-output failure preservation. Source rejection categories and program
behavior must match v1; native binary bytes need not match the whole-project
binary. Repeated separate builds with identical explicit inputs and the same
toolchain must be byte-deterministic.

Checks cover `x86_64` and `wasm32`; native object/link tests cover supported
host x86-64 pipelines. Existing direct/v1 wasm32 LLVM IR output remains
supported. Separate LLVM IR output and a wasm native runtime/linker are not
added by this proposal.

## Alternatives and review boundaries

- **Object files alone:** insufficient to type-check imports or validate
  visibility, layouts, and interface contracts.
- **Persist MIR or LLVM IR as the public artifact:** adds a serialized
  implementation/IR boundary and repeats native lowering at link time. The
  proposed semantic/ABI plus native-object boundary avoids requiring either.
- **Metadata/object sidecars:** introduce pair-mismatch and multi-file
  publication problems; one envelope keeps publication and integrity coupled.
- **Rebuild dependencies inside the consuming compiler:** preserves the
  current whole-project limitation rather than establishing separate units.
- **ABI-compatible reuse based only on a package version:** cannot detect a
  local edit without a version bump; exact dependency digests are required.

Approval covers artifact kinds/envelope, exact compatibility,
canonical ownership and ABI revision, process v2, and the explicit caching and
target limits. Implementation-library selection and native object-inspection
mechanics remain engineering choices within this contract, with any new build
dependency documented before it is introduced. Any change to the proposed
serialized or process contract must return to design review, not be inferred
from implementation convenience.
