# Canonical compiler identity

Stage 23.2 separates persistent identity from display names and compilation-local
handles. This document describes the implemented identity/ownership foundation;
the approved [artifact contract](proposals/stage_23_artifacts.md) also schedules
serialization. Owned imported declaration/ABI views are documented in
[imported package views](imported_package_views.md).

## Identity inputs

`FileSemantics::identity` owns the manifest package name and exact version,
source-package components, file stem, and class/interface kind. The source-loading
path carries the version from Shuttle's validated request through syntax to
semantics. Invalid owning package identities and two versions of the same owning
package are diagnosed. Standalone compilation has a distinct owner domain with
empty package name and version.

Dependency aliases, source paths, local `FileId`/`TypeId`/`SymbolId` values, and
filesystem enumeration order do not participate in canonical identity. Source
names and `::typeName` remain unchanged; version-qualified identity is not a new
source namespace or display spelling.

## ABI revision 2 encoding

`component(bytes)` is an unsigned little-endian 64-bit byte length followed by
the exact bytes. A list is an unsigned little-endian 64-bit element count
followed by its elements. Encodings contain binary NUL bytes and must not be
treated as C strings. No host structure representation is serialized.

Nominal identity is this ordered sequence:

1. `component("nominal")`;
2. `component("package")`, or `component("standalone")` for direct compilation;
3. package name and exact version, each encoded as a component;
4. the source-package component list, each segment encoded separately;
5. the file stem as a component; and
6. `component("class")` or `component("interface")`.

Primitive identity is `component("primitive")` followed by a component
containing the canonical primitive name. `int`, `uint`, and `float` resolve to
`int32`, `uint32`, and `float32` first. An array or nullable identity is
`component("array")` or `component("nullable")`, followed by one component
containing the encoded element identity. This retains nullability in semantic
records. Overload identity recursively erases nullable wrappers, preserving the
existing language rule.

A member identity contains its domain tag, encoded nominal owner, source name,
and parameter-identity list. Each of the first three values is a component;
every parameter is a component containing its encoded overload type identity.
The exact member domain tags are:

| Member | Tag |
| --- | --- |
| Function | `function` |
| Allocating constructor | `constructor` |
| Constructor initialization entry | `constructor-initializer` |
| Static field | `static-field` |
| Instance field | `instance-field` |
| Class descriptor | `descriptor` |

Fields and descriptors have empty parameter lists. A descriptor has an empty
member name. Return types and modifiers are checked signature information, not
overload discriminators. Constructor source spelling is preserved independently
of the owner identity.

Native Cloth symbols are `_C2` followed by lowercase hexadecimal encoded member
identity bytes. The encoding is injective and never truncates names to a hash.
It replaces `_C1` callable/static names and module-index descriptor names on
both direct and protocol-v1 paths. There is no promise of ABI compatibility with
previous compiler builds. Internal field-initializer helper names remain
module-local implementation details, not persistent ABI symbols.

## Interface IDs

Interface IDs use FNV-1a over the complete canonical nominal bytes, with offset
`14695981039346656037`, prime `1099511628211`, and arithmetic modulo 2^64. The
existing semantic collision diagnostic remains in force. Table entries remain
ordered by ID and methods by the established contract-slot order. Exact package
version, including SemVer build metadata, participates in the ID.

`tests/unit/identity_tests.cc` fixes primitive and nominal byte encodings and an
interface hash vector independently of the encoder. It also checks source-order,
path, and alias independence, distinct domains, exact versions, nullability,
primitive aliases, package validation, and ABI corruption rejection.

## Definition ownership

Each class descriptor has its canonical external symbol in the ABI model.
Generated code defines it once, with no mergeable-address annotation. Descriptor
backing tables and literal bytes remain private. Nominal descriptor linkage
does not change capitalization-based source visibility.

Constructor initialization linkage is explicit in `AbiCallable`. Accessible
constructors expose their initialization entry for cross-package base calls;
private constructors keep both entries internal. The initialization entry still
accepts the existing object and returns void; it never allocates a second object
or becomes a source-level member. ABI verification checks the descriptor names
and initialization linkage against reconstructed expectations.

The internal `LlvmIrOptions::package` selector emits definitions only for the
selected manifest package, with external declarations for dependency descriptors,
public callables, accessible constructor initializers, and public static storage.
Private dependency implementation bodies are not emitted. Unknown owners and
package requests with entry-wrapper options are rejected.

The backend partition and imported-view extractor currently consume a verified
whole-project semantic/MIR/ABI graph. The extracted view is detached and
source-free, and the version-1 `.cpa` reader/writer preserves it. Protocol-v2
commands and dependency-closure loading remain Stage 23.3 work. The runtime
descriptor representation, GC root frames, object layout, and native entry
behavior remain unchanged.
