# Stage 50: Self-hosted semantic identity and name resolution

Status: **complete — 50.4 authority audit passed 2026-09-12**.

Stage 50 builds the first package-semantic layer on the authoritative
self-hosted frontend. It establishes deterministic symbols and type identities,
collects package declarations, and resolves imports and type names without
returning ownership to the temporary single-file path.

See the [compiler roadmap](../../ROADMAP.md), [work ledger](../../TODO.md),
[package and import rules](../packages_and_imports.md),
[canonical identity](../canonical_identity.md), and
[Stage 49 frontend authority contract](stage_49_self_hosted_frontend_authority.md).

## 1. Authority and scope

The self-hosted package frontend remains authoritative for lexing and parsing.
Stage 50 consumes its immutable `PackageFrontendResult`; it does not re-lex,
reparse, reconstruct syntax from diagnostics, or create a second file model.

The frozen C++ semantic implementation remains the behavior and differential
oracle through 50.4. It receives no new language features. Stage 50.4 may
transfer authority only for package symbols, canonical semantic type identity,
import binding, and declared type-name resolution. Expression typing, overload
selection, inheritance, effects, flow analysis, HIR, and all later phases remain
with the bootstrap until their own authority checkpoints.

Stage 50 changes no source syntax. It implements the existing package,
capitalization, standard-library, object, nullability, and canonical-identity
contracts in Cloth.

## 2. Semantic input boundary

`SemanticPackageInput` contains:

- one accepted `PackageFrontendResult` for the package being analyzed;
- an immutable sequence of direct dependency bindings; and
- the compiler-owned core type catalog.

Each dependency binding contains a source-visible alias, an exact package name
and version, and one already verified, detached declaration view. Artifact
decoding and closure verification occur before this boundary. The semantic
layer never trusts raw artifact bytes, opens dependency sources, reads a
manifest, walks a directory, consults `PATH`, or accesses the network.

The distinguished `cloth` dependency is supplied through the same verified
view boundary and must match the compiler-paired package exactly. A standalone
input retains the separate empty-owner domain established in Stage 49.

Frontend failure remains isolated by file. A lexically invalid file has no
trusted tree and contributes no semantic declarations. A parser-invalid file
may contribute valid recovered declarations from its verified tree. Independent
files continue through semantic construction. Frontend diagnostics are retained
once and precede semantic diagnostics; Stage 50 does not duplicate or replace
them.

Dependency aliases are lookup spellings, not identity inputs. Direct
dependencies alone are source-visible. Their transitive declaration closure may
be present for verification and layout, but it does not create an import path.
Aliases cannot collide with a local top-level source package, and only the exact
distinguished standard-library edge may claim `cloth`.

## 3. Identity layers

Stage 50 keeps three concepts separate:

1. A display name is source-facing text such as `User` or `models.User`.
2. A result-local handle is a checked integer index into one immutable semantic
   result. File, type, and symbol handles never cross that result boundary.
3. A persistent identity is an owned structural value used across packages,
   artifacts, ABI records, and independent compiler runs.

Pointer identity, allocation order, dependency aliases, import aliases,
absolute paths, checkout location, source enumeration order, and local handles
never determine semantic equality. Identity comparison is value comparison.

Handles are allocated deterministically. Local files use the canonical Stage 49
file order and declarations retain source order inside their owner. Dependency
packages and detached declarations use canonical persistent-identity order;
alias declaration order cannot perturb handles or diagnostics.

## 4. Type identity

A nominal identity owns this exact tuple:

```text
owner domain:     package | standalone
package:          exact name and SemVer, or both empty for standalone
source package:   ordered identifier components
file type:        file stem
nominal kind:     class | interface | enum | struct
```

An `error` file uses the class nominal tag; its error semantics come from its
kind and ancestry. Package version, including prerelease and build metadata,
participates in identity. Source spelling and import display omit that version.

The semantic type universe contains:

- recovery-only error and bottom types;
- `void` and the null-literal type;
- canonical `bool`, `char`, `byte`, fixed-width integer, fixed-width floating,
  and `string` types;
- the compiler-owned `Error` root;
- the exact `cloth.lang.Object` nominal type selected by the `object` keyword;
- one nominal type for every class, error, interface, enum, and struct;
- one structural array identity per element type; and
- one structural nullable identity per permitted underlying type.

`int`, `uint`, and `float` normalize to `int32`, `uint32`, and `float32` before
identity is formed. Source aliases never create a type. `void`, the null-literal
type, and an already nullable type cannot be wrapped in nullable identity;
arrays cannot have `void` elements. Primitive, enum, struct, reference, string,
object, and array types otherwise retain the Stage 41 nullable rules.

Semantic identity preserves every nullable wrapper. Callable overload identity
recursively erases nullable wrappers, preserving Cloth's rule that overloads
cannot differ only by nullability. Arrays remain structural and invariant;
their element identity remains present after overload erasure.

Stage 50 reuses compiler ABI 7 canonical identity encoding. It does not define a
text concatenation, hash, or mangled name as semantic equality. Recovery types
and result-local handles cannot be serialized as declared type identities.

## 5. Symbols and visibility

The semantic model distinguishes file types, fields, functions, constructors,
parameters, locals, `self`, interfaces, enums, enum cases, structs, and errors.
Only file-owned declared types and members receive persistent identities.
Parameters, locals, `self`, compiler recovery symbols, and intrinsics use
result-local handles only.

A persistent member identity contains its nominal owner, member domain, owned
member name, and callable parameter overload identities. Functions, fields,
and enum cases retain their declared name. The constructor domain uses its
owner's file-type name; its independently declared source spelling is retained
as diagnostic and visibility data. Field staticness has a distinct member
domain. Return types, visibility, `final`, `static`, `override`, `abstract`,
throws clauses, source ranges, declaration order, and implementation bodies are
verified properties, not overload discriminators.

Constructor visibility is inferred from its declared source spelling, while
constructor overload identity uses the owner and parameter types. Therefore
`User(...)`, `user(...)`, and `_User(...)` cannot coexist when their parameter
overload identities are otherwise equal. The spelling and inferred visibility
remain available for diagnostics and access checks.

Visibility follows the first ASCII character:

- `A` through `Z` is public;
- lowercase and `_` declarations are private; and
- enum cases are public regardless of spelling.

A private file type is accessible only within its own file. A private member or
constructor is accessible only within its owning file type. Same-source-package
placement does not widen private access. Imported views retain private records
for verification but never expose them through source lookup.

## 6. Import and type-name resolution

Imports are file-scoped type bindings. They never include text, execute code,
import bare members or enum cases, re-export a declaration, or discover a file.
Import cycles are valid because all nominal types are registered before any
type syntax is resolved.

Existing forms retain their meaning:

```cloth
import models::User;
import models::User as ModelUser;
import services.api::*;
import RootType;
```

`.` traverses source-package components, `::` selects one implicit file type,
`.*` selects every public type directly in one source package, and `as` changes
only the file-local lookup spelling. Wildcards are explicit and nonrecursive.

For a dependency import, the first component is a direct dependency alias. For
a local import, it is a local source-package component. The input boundary
rejects collisions between those namespaces, so resolution does not guess.
An exact imported declaration must match the dependency binding's verified
package identity; an alias cannot rewrite nominal ownership.

Unqualified declared type names resolve in this order:

1. the current file type;
2. public types in the current source package;
3. explicit imports and explicit aliases;
4. explicit wildcard imports;
5. unique public types recursively beneath the exact `cloth.lang` prelude; and
6. compiler-owned core type names.

An exact import wins over a wildcard. Two applicable wildcards providing the
same name are ambiguous unless an exact import or alias supplies that name.
Higher-priority bindings shadow prelude names without warning. Public prelude
short names must be unique across `cloth.lang`; an invalid standard-library
view is rejected before user resolution.

Every `TypeSyntax` in file headers, field and callable signatures, parameters,
throws clauses, constructor initializers, local declarations, conversions, and
type tests is resolved through the same file import scope. Stage 50 records the
resolved type handle or a recovery handle and diagnostic. It does not select a
callable, bind value expressions, or decide assignment compatibility.

## 7. Registration and resolution order

Semantic construction is ordered and explicit:

1. validate the semantic input and dependency closure;
2. install core structural types and the exact standard-library roots;
3. register every local and imported nominal type;
4. build each file's immutable import scope;
5. register file-owned member declarations and stable member handles;
6. resolve all declared type syntax; and
7. verify and publish one immutable package result.

Registration never depends on a declaration body. Forward type references and
import cycles therefore work without retries. Duplicate or ambiguous names are
retained as invalid records for diagnostics but are never resolved by choosing
the first declaration.

Indexes are built once and queried without rescanning all files for every name.
Canonical sorted tables and bounded binary or merge lookup are preferred over
hash iteration so observable behavior cannot depend on a runtime seed.

## 8. Result ownership

`SemanticPackageResult` owns:

- its complete `SemanticPackageInput` and authoritative frontend result;
- immutable type, symbol, file, import-scope, and type-binding tables;
- structured semantic diagnostics; and
- aggregate validity.

The result is valid exactly when the frontend result is valid and semantic
construction reports no error. An accepted but invalid package still publishes
a complete verified recovery result for diagnostics and later tooling.
Consumers cannot mutate caller arrays, internal tables, source names,
identities, or diagnostic order.

All source-backed ranges remain valid because the semantic result retains its
frontend owner. Detached dependency views own their strings and records.
Published objects contain no borrowed native pointers. Future garbage
collection may reclaim the graph only when no semantic result or downstream
phase retains it.

Every public handle accessor checks its result ownership and bounds. A foreign,
negative, missing, or forged handle is a `StateError`, never an arbitrary array
access or silently rebound symbol.

## 9. Diagnostics and recovery

Semantic failures use exhaustive `SemanticDiagnosticKind` values with primary
source ranges and optional related declaration ranges. Message selection and
rendering remain separate from symbols and type identities. Missing message
mappings are compiler errors.

Diagnostics follow canonical file order, then semantic phase order, then source
order with a stable kind tie-break. A primary diagnostic's related locations
remain adjacent. Dependency diagnostics use package-relative declaration
locations and exact owning package identity; they never reveal an artifact or
cache path.

Required coverage includes duplicate declarations and overloads, invalid
constructor collisions, unknown and inaccessible types, missing imports,
invalid aliases, exact-import conflicts, wildcard ambiguity, malformed imported
views, standard-library identity failures, and foreign semantic handles.
Recovery prevents cascades but never converts ambiguity or inaccessibility into
a successful binding.

## 10. Determinism, resources, and security

The result depends only on the declared frontend result, exact dependency
bindings and verified views, compiler compatibility constants, and core
catalog. It is independent of current directory, absolute paths, timestamps,
locale, environment, filesystem behavior, input order, allocation addresses,
and map iteration.

Counts, cumulative name bytes, structural type depth, import expansion, symbol
tables, diagnostics, and canonical encodings use named budgets and checked
integer arithmetic. Stage 50.2 must set concrete implementation limits before
allocation. Exceeding a limit produces one deterministic resource diagnostic
or input rejection; it does not rely on allocation failure or recursion depth.

Structural type construction, imported closure verification, and retained graph
verification are iterative or explicitly depth-bounded. Wildcard and prelude
indexes are constructed once. Invalid dependency data cannot forge the local
package owner, expose a private declaration, claim `cloth`, or inject a host
path into diagnostics.

## 11. Implementation organization

Production code belongs under responsibility-specific directories:

```text
src/semantic/
  identity/     persistent nominal, structural, and member identity
  model/        checked handles and immutable semantic records
  package/      semantic input, result, and package coordinator
  symbol/       declaration registration and indexes
  resolution/   import scopes and declared type-name resolution
  diagnostic/   semantic kinds and message catalog
```

Files may be added only when they own a named invariant. The driver constructs
the input and renders the result; it does not own semantic tables or resolution
rules. Tests consume these production components through ordinary package
dependencies and do not introduce a test-only semantic implementation.

## 12. Verification and authority transfer

Stage 50 verification covers:

- every type and symbol kind, primitive alias normalization, nominal package
  versions, structural arrays and nullability, and overload erasure;
- capitalization visibility, enum cases, constructor spelling, ownership, and
  local-versus-persistent handles;
- local, exact, aliased, wildcard, dependency, prelude, and core resolution;
- duplicates, ambiguity, inaccessibility, malformed imported views, and exact
  diagnostic records;
- shuffled declarations and dependency bindings, relocated checkouts, repeated
  and parallel runs, and byte-identical published records;
- retained results under collection pressure and rejection of foreign handles;
- the complete production self-hosted source package and focused valid and
  invalid differential corpora against the frozen C++ oracle;
- x86-64 and wasm32 checking, Bazel presubmit/full, Shuttle compatibility,
  formatting, documentation, and repository hygiene.

Stage 50.4 records measured corpus counts and every gate. Any unexplained
identity, visibility, import, binding, recovery, diagnostic, ordering, resource,
or lifetime difference blocks authority transfer.

## 13. Checkpoint plan

1. **50.1 — Semantic model contract (complete).** Freeze authority, inputs,
   identity layers, type and symbol domains, visibility, import lookup,
   deterministic construction, result ownership, diagnostics, resources,
   organization, compatibility, verification, and non-goals.
2. **50.2 — Package symbols and type identity (complete).** Implement checked local
   handles, persistent identities, immutable semantic inputs/results, core and
   nominal type registration, declaration symbols, deterministic indexes,
   validation, diagnostics, and focused GC/resource tests.
3. **50.3 — Imports and declared type-name resolution (complete).** Implement local,
   dependency, alias, wildcard, prelude, and core scopes; bind every declared
   `TypeSyntax`; preserve recovery and related diagnostics; and add exhaustive
   valid, invalid, and differential coverage.
4. **50.4 — Integration and authority audit (complete).** Close production-package,
   dependency-view, malformed, determinism, resource, GC, target,
   cross-compiler, Bazel, Shuttle, documentation, and repository gates; then
   transfer authority for the Stage 50 semantic boundary.

### 50.2 implementation record

Production code now occupies the approved `src/semantic` ownership tree. File,
type, and symbol handles carry an opaque result owner and checked index; public
records can test ownership but cannot reveal the token needed to manufacture a
handle for an existing result. Immutable package inputs copy and sort direct
dependency bindings, retain the Stage 49 frontend result, accept only normalized
package-relative dependency locations, and reject alias, owner, standard-
library, declaration, and resource violations before allocation.

The collector installs the fixed 22-entry core catalog, including recovery,
bottom, `void`, null, primitives, compiler-owned error roots, and nullable
`Error`. It then registers every trusted local and detached nominal declaration
before registering source members. Exact `cloth` v0.6.0 `lang.Object` reuses the
`object` core handle; malformed dependency claims are rejected and malformed
local claims receive structured diagnostics. Nominal and member indexes use
canonical value ordering and bounded binary lookup. Duplicate member identities
remain ambiguous instead of selecting the first declaration.

The concrete implementation limits are 65,536 local files, 4,096 direct
dependencies, 262,144 detached dependency types, 524,288 semantic types,
1,048,576 symbols, 262,144 semantic diagnostics, 16,777,216 retained name
bytes, and 256 structural identity layers. Focused success, GC, resource,
malformed-view, path, alias, determinism, and foreign-handle tests pass. The
expanded differential closure passes 664 lexer inputs, 32 bounded and 226
production declaration inputs, and 43 bounded and 226 production definition
inputs. The complete 20-target Bazel presubmit passes, and the production
self-hosted compiler target builds successfully.

Function and constructor member identities require resolved parameter types and
therefore finalize in 50.3. Import scopes, `TypeSyntax` bindings, driver routing,
semantic authority transfer, and removal of the C++ oracle are not part of
50.2.

### 50.3 implementation record

The collector now builds one immutable scope for every trusted local file.
Current-file and current-package declarations, exact and aliased imports,
nonrecursive wildcards, direct dependency aliases, the recursive exact
`cloth.lang` prelude, and core aliases resolve in the contracted precedence.
Canonical file and name indexes provide bounded binary lookup. Import events
retain phase and source order for deterministic diagnostics, while one stable
sort prevents quadratic scope merging. Unknown, private, conflicting, and
ambiguous imports remain explicit recovery bindings without selecting a target.

Every `TypeSyntax` in file headers, fields, callable parameters and results,
throws clauses, constructor initializers, local and foreach declarations,
conversions, array construction, and type tests now publishes a result-owned
binding. Preflight and binding share the same exhaustive definition-type
classification, so capacity is based on actual type positions. Array and
nullable identities are structurally interned. Callable identities finalize
from resolved parameter identities with recursive nullable erasure; duplicate
functions and constructors are diagnosed without choosing a declaration.

Focused valid and invalid packages cover lookup precedence, all file-type
kinds, capitalization privacy, dependency aliases, prelude recursion, core
aliases, structural types, every body type position, overload identity,
recovery, diagnostic order, shuffled inputs, foreign handles, and retained
sources under collection pressure. Ten independently named semantic failure
contracts pass. The complete 21-target presubmit and 95-target full suite pass,
and exact differential parity covers 693 lexer inputs, 32 bounded and 247
production declaration inputs, and 47 bounded and 247 production definition
inputs. Driver routing and semantic authority transfer remain 50.4; C++
semantic deletion remains outside Stage 50.

### 50.4 implementation record

The production driver now sends every valid Stage 49 package result through
`SemanticPackageInput` and `PackageSymbolCollector`, then renders frontend and
semantic diagnostics through one terminal-safe presentation boundary. The
driver retains orchestration and process-status ownership only; semantic state
remains owned by the immutable package result.

The frozen C++ compiler gained only a test oracle. Canonical semantic file,
member, type, visibility, modifier, callable-identity, and diagnostic records
match the Cloth implementation for focused valid and invalid inputs. Repeated,
relocated, and parallel Cloth records are byte-identical. The complete 248-
source compiler package resolves with five detached standard-library types and
publishes 253 semantic files, 410 types, and 3,314 symbols.

The final differential corpus contains 697 lexer inputs, 32 bounded and 248
production declaration inputs, 47 bounded and 248 production definition
inputs, and two semantic inputs. All 355 development and 355 Clang ASan/UBSan
CTests pass. All 22 Bazel presubmit targets pass, and an uncached full run
passes all 96 targets in 423.4 seconds. Its exclusive Shuttle audit passes in
115.8 seconds and its cross-compiler audit passes in 206.7 seconds, preventing
package-scale audits from competing for CPU. x86-64 cold/warm construction,
native execution, wasm32 checking, failed-output preservation, malformed,
resource, ownership, and GC gates all pass.

The complete production package additionally checks through the Clang
ASan/UBSan bootstrap compiler from an isolated staging tree: x86-64 completes
in 23.58 seconds and wasm32 in 23.97 seconds. This covers the newly authoritative
Cloth semantic sources under instrumentation on both supported targets.

Authority transfers to the self-hosted implementation for package symbols,
canonical semantic type identity, import binding, and declared type-name
resolution. The frozen C++ compiler remains the bootstrap and differential
oracle for expression typing, HIR, and all later boundaries. Stage 50 does not
delete C++ semantic code or expand the transferred scope.

## 14. Compatibility

Stage 50.1 changes documentation only. Stage 50 adds compiler-internal managed
types and diagnostics but no Cloth grammar, artifact format, compiler ABI,
runtime ABI, standard-library source version, editor grammar, Shuttle manifest,
compiler-process, receipt, or toolchain-metadata schema change. Compatibility
remains artifact/compiler/runtime **8/7/12**, schemas **2/1/1/1**, and
compiler-paired `cloth` **v0.6.0**.

Persistent identities must reproduce the existing compiler ABI 7 encoding.
Changing that encoding or the imported package declaration schema requires a
separate compatibility review and is not implicit in Stage 50.

## 15. Non-goals

Stage 50 does not add syntax, file or dependency discovery, manifest parsing,
artifact byte decoding, package solving, remote retrieval, expression value
binding, member access or call selection, overload conversion ranking,
inheritance or interface conformance, constant evaluation, effect checking,
definite assignment, null-flow refinement, control-flow analysis, typed HIR,
MIR, ABI layout, LLVM lowering, code generation, linking, optimization,
incremental caches, a language server, C++ semantic deletion, or semantic
authority beyond the explicitly audited Stage 50 boundary.
