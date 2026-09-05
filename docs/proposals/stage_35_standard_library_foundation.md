# Proposal: Stage 35 standard library foundation

Status: **complete — exit audit passed 2026-09-05**.

This proposal establishes the official Cloth standard library as a versioned,
verified part of the Cloth toolchain. The library source is owned by the `std`
repository, compiled through ordinary Cloth package boundaries, and supplied to
applications by Shuttle without manifest boilerplate or namespace ambiguity.

See the [compiler roadmap](../../ROADMAP.md#stage-35-standard-library-foundation),
the [compiler work ledger](../../TODO.md#stage-35-standard-library-foundation),
and the standard-library repository's [roadmap](../../std/ROADMAP.md).

## 1. Ownership

The `std` submodule is the source of truth for standard-library Cloth code.
Reusable APIs belong there rather than in the compiler or native runtime when
they can be expressed faithfully in Cloth.

The compiler continues to own language primitives and contracts required to
compile without the standard library:

- primitive types, `object`, arrays, and `void`;
- the compiler-provided `Error` root and `DivisionByZero`;
- allocation, tracing, error propagation, and physical ABI rules;
- primitive meta operations; and
- the existing `print` and `println` compatibility surface.

The standard library owns reusable source-level facilities such as mathematics,
text processing, parsing, collections, console and file APIs, and domain error
types. A later API stage may add those facilities incrementally. Stage 35 does
not move `Error` into the library or add console input, parsing, collections, a
prelude, or another language primitive.

## 2. Reserved `cloth` root

`cloth` is the exact lowercase standard-library root in source imports:

```cloth
import cloth.math::Math;
```

`.` separates source-package components and `::` selects the implicit file
type. Existing alias and wildcard forms remain valid beneath the root:

```cloth
import cloth.math::Math as StandardMath;
import cloth.text.*;
```

Ordinary code cannot define or redirect the `cloth` root. The following are
rejected with targeted diagnostics:

- a top-level ordinary source directory named `cloth`, including case-only
  filesystem variants;
- a user-declared Shuttle dependency alias named `cloth`;
- a user import alias named `cloth`; and
- an ordinary package graph that attempts to replace the selected standard-
  library package identity.

Wildcard imports remain nonrecursive, imports remain file-scoped, and standard-
library imports never re-export members. Capitalization retains its existing
visibility meaning. The reservation introduces no `module` declaration and no
second import grammar.

The reservation prevents accidental or manifest-directed shadowing. Package
signing and hostile-registry provenance are separate distribution concerns and
are not claimed by Stage 35.

## 3. Package identity and repository layout

The official Shuttle package identity is `cloth`. Its package version is
independent of the language version, but a released toolchain selects one exact
version and content digest.

The package source root does not repeat its owning package name:

```text
std/
  Shuttle.toml
  src/
    math/
      Math.co
```

Consequently, `src/math/Math.co` has canonical nominal identity
`cloth.math.Math` and is imported as `cloth.math::Math`. The current bootstrap
path `src/cloth/math/Math.co` must move during 35.2; retaining it would produce
the duplicated identity `cloth.cloth.math.Math`.

The standard library is a library package and has no `[executable]` target.
Applications and dedicated fixtures provide entry points when native behavior
must be tested. Standard-library source filenames, directories, declarations,
visibility, and imports obey the same language rules as every other package.

## 4. Availability and imports

Shuttle supplies the selected standard library as a direct, implicit dependency
of every ordinary package. Users do not add a `cloth` entry to
`[dependencies]`. This implicit graph edge makes `cloth` imports available in
each package without making any standard-library type automatically visible.

There is no implicit wildcard import or general prelude. A source file must
explicitly import a standard-library type unless a future approved prelude
places that exact declaration in scope. Compiler-owned core symbols retain
their existing always-visible behavior.

The standard-library package itself is compiled in bootstrap mode without a
self-dependency. It may import its own sibling source packages through ordinary
package-relative imports. Cycles retain the existing declaration-first package
semantics; initialization behavior is not added by this stage.

Direct `clothc` use remains core-only unless the caller supplies the standard
library through the compiler's explicit package-build boundary. The compiler
does not search the current directory, environment variables, user home, or
network for standard-library source. User-facing standard-library builds use
Shuttle.

## 5. Shuttle and compiler boundary

Shuttle owns selection, installation layout, graph injection, progress, cache
policy, and toolchain diagnostics. `clothc` owns reserved-root validation,
source import resolution, semantic identity, artifact validation, and code
generation.

Shuttle passes the standard library through the existing explicit dependency
boundary. It does not parse library Cloth source, enumerate exported APIs, or
adapt ABI metadata. The compiler treats the library as an ordinary verified
package after validating the distinguished `cloth` identity.

Every package artifact and build receipt records the exact `cloth` package
version and digest through the existing dependency inventory. A standard-
library edit or selection change invalidates consumers conservatively. An
unchanged verified library remains eligible for Stage 24 reuse. Failed library
compilation or validation cannot publish a replacement artifact, link stale
output, or run a previous executable.

Selecting a compiler selects its compatible standard-library distribution.
Shuttle does not independently choose the newest installed library or solve a
version range. Missing, duplicate, incompatible, malformed, or shadowed
standard-library inputs fail before application compilation.

## 6. Compatibility

Stage 35 reuses the existing package and dependency representation. Artifact
format **5**, compiler ABI **5**, runtime ABI **4**, process protocol **2**,
receipt schema **1**, and manifest schema **1** remain unchanged unless 35.2
finds an invariant that cannot be represented safely. Such a finding requires
an explicit contract amendment before implementation continues.

The standard-library package version and digest are nevertheless exact build
inputs. There is no promise that library binaries compiled by a different
compiler, compiler ABI, runtime ABI, target, or native toolchain are reusable.
Existing artifact validation remains authoritative.

Projects that currently own a top-level `cloth` source package or dependency
alias must rename it. The compiler and Shuttle issue a direct reserved-root
diagnostic rather than allowing later ambiguous-import or duplicate-identity
failures.

## 7. Distribution

Source development occurs in the `std` repository. A released Cloth toolchain
bundles one exact compatible standard-library distribution and the metadata
needed for Shuttle to locate and verify it without network access. Development
builds select the checked-out submodule explicitly through test configuration;
they do not depend on a machine-global installation.

The package is compiled independently for each supported target and artifact
kind. Interface artifacts remain target-checked compiler products; object
artifacts contain target-specific native payloads. Applications link only
compiler-produced artifacts and never copy or compile library files through a
textual include mechanism.

Remote registries, automatic downloads, semantic-version solving, lockfiles,
package publication, signing, and system-wide installation policy remain
deferred. Stage 35 establishes the local and bundled toolchain contract those
features must later preserve.

## 8. Verification and diagnostics

Verification must cover:

- exact `cloth.math::Math` import and canonical identity;
- explicit imports, aliases, and nonrecursive wildcards beneath `cloth`;
- local-directory, dependency-alias, import-alias, package, and case-collision
  shadow attempts;
- absence, duplication, corruption, version mismatch, target mismatch, and
  compiler/runtime incompatibility;
- standard-library bootstrap without an executable or self-dependency;
- whole-project, separate-package, and source-free equivalence;
- x86-64 native behavior and verified x86-64/wasm32 LLVM;
- deterministic relocated one-job and parallel artifacts;
- exact invalidation, reuse, atomic publication, and stale-output prevention;
  and
- compiler, runtime, Shuttle, standard-library, editor, documentation,
  formatting, link, and sanitizer gates.

Diagnostics name the owning layer and preserve source locations. Namespace and
import violations belong to the compiler when observed in source; manifest and
toolchain-selection violations belong to Shuttle. Neither layer converts a
missing standard library into an unknown cascade of individual types.

## 9. Stage plan

1. **35.1 — Contract (complete).** Freeze ownership, reserved-root behavior, package
   layout, explicit imports, automatic dependency injection, compatibility,
   distribution, diagnostics, verification, and non-goals.
2. **35.2 — Compiler and library bootstrap (complete).** Enforce the reserved root,
   normalize the `std` package and source layout, compile `Math` as the first
   library component, and verify interface/object artifacts and both targets.
3. **35.3 — Shuttle integration (complete).** Select and inject the toolchain library,
   integrate deterministic reuse and linking, document the workflow, and prove
   source-free consumer behavior.
4. **35.4 — Exit audit (complete).** Close namespace, compatibility, corruption,
   determinism, invalidation, distribution, native, cross-target, and repository
   quality matrices.

All four checkpoints are complete.

### 35.2 implementation record

The compiler now rejects case-insensitive `cloth` collisions in source-package,
owning-package, import-alias, and dependency identities, while accepting only
the exact distinguished dependency `cloth -> cloth`. The standard library is
the executable-free `cloth` v0.1.0 package, with `src/math/Math.co` retaining
the canonical identity `cloth.math.Math`. Its dynamic integer operations retain
Stage 34 safety through explicit `DivisionByZero` declarations.

Compiler tests build standard-library interfaces for x86-64 and wasm32, verify
whole-project LLVM before and after O2 on both targets, and build, consume, and
link independent native object artifacts without reopening dependency source.
Both development and ASan/UBSan configurations pass all 255 CTests. No
compatibility version changed.

### 35.3 implementation record

The compiler now advertises its exact `cloth` package and version. CMake emits
an adjacent schema-1 `cloth-toolchain.json` that selects the checked-out
standard-library manifest by relative path; the same contract supports a
bundled release layout without working-directory, environment, home-directory,
or network discovery.

Shuttle validates that metadata and its executable-free, dependency-free
distribution, injects `cloth -> cloth` directly into every ordinary package,
and compiles the library without a self-edge. User `cloth` dependency aliases
and package replacements fail before application compilation. The ordinary
artifact graph carries the exact library version and digest through progress,
reuse, invalidation, linking, and receipts. x86-64 and wasm32 checks, native
execution of `cloth.math::Math`, and warm reuse pass without manifest
boilerplate. Compatibility remains artifact/compiler/runtime 5/5/4 with
process/receipt/manifest schemas 2/1/1; toolchain metadata begins at schema 1.

### 35.4 exit record

The exit audit closes exact, aliased, and nonrecursive wildcard imports beneath
`cloth`; case-insensitive source, import, dependency, and package shadowing;
strict metadata and distribution failures; executable-free bootstrap; and
whole-project, separate-package, and source-free consumers. Invalid or duplicate
metadata fields, unsupported schemas, mismatched selections, non-normal paths,
missing manifests, and wrong-name, wrong-version, executable, or dependency-
bearing distributions all fail at the Shuttle boundary before compilation.

Standard-library edits invalidate `cloth` and exact consumers on x86-64 and
wasm32, while warm artifacts remain reusable. Corrupt candidates are repaired
without invalidating byte-identical consumers. A failed library edit preserves
the last completed library artifact, consumer artifact, and executable and does
not launch stale output. Relocated one-job/four-job artifacts and native
executables remain byte-identical.

Development and ASan/UBSan configurations each pass all 255 CTests, including
35 public Shuttle toolchain cases and 32 native cases. All 49 ordinary Rust
tests, Rust 1.85, warning-denied Clippy, Rust and C++ formatting, compiler-backed
editor tests, documentation links, and repository checks pass. Compatibility
remains 5/5/4 with process/receipt/manifest/toolchain schemas 2/1/1/1.

## 10. Non-goals

Stage 35 does not add:

- console input, command-line argument delivery, primitive parsing, formatting,
  collections, filesystem, networking, threading, or platform APIs;
- an implicit prelude, implicit wildcard imports, re-exports, module
  declarations, or an alternate namespace separator;
- source-defined replacements for compiler-owned primitives or `Error`;
- native plugin, build-script, foreign-function, or arbitrary command execution;
- registries, downloads, version solving, lockfiles, publication, or signing;
- new language syntax, callable behavior, memory semantics, optimization, or
  target support; or
- a Cloth 1.0 stability promise for the library API.
