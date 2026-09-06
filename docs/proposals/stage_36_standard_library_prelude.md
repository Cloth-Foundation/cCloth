# Proposal: Stage 36 standard-library prelude

Status: **complete — 36.4 exit audit passed 2026-09-06**.

This proposal defines the `cloth.lang` namespace tree as Cloth's focused
standard-library prelude. It makes foundational library types convenient without
turning library source into compiler intrinsics, introducing textual includes,
or implicitly opening the complete `cloth` namespace.

See the [compiler roadmap](../../ROADMAP.md#stage-36-standard-library-prelude),
the [compiler work ledger](../../TODO.md#stage-36-standard-library-prelude),
and the standard-library repository's [roadmap](../../std/ROADMAP.md).

## 1. Availability and visibility

Stage 35 made the compiler-paired `cloth` package available to every ordinary
Shuttle package. Availability is a build-graph property: the library is
selected, compiled, and passed to the compiler as a verified dependency.

A prelude is a separate name-resolution property. When the canonical `cloth`
package is present in a compilation, every source file may refer to eligible
types beneath `cloth.lang` without writing an import:

```cloth
func Validate(int32 value) throws ArgumentError {
  if (value < 0) { throw ArgumentError("value must be non-negative"); }
}
```

`ArgumentError` is an ordinary standard-library declaration resolved through
the prelude, not a compiler-owned error or keyword.

The prelude is a compiler-managed recursive lookup across the `cloth.lang`
namespace tree, but it is not inserted into the AST as a source import.
It does not copy library source into a consumer, create a second dependency,
or change the package/link graph established by Stage 35.

## 2. Canonical namespace and layout

The only implicit standard-library namespace tree is `cloth.lang` and its
descendant source packages. Standard-library files map normally from the
existing source root:

```text
std/
  src/
    lang/
      errors/
        ArgumentError.co
        StateError.co
```

`src/lang/errors/ArgumentError.co` has canonical identity
`cloth.lang.errors.ArgumentError`. The name
`lang` is not a keyword or a module declaration. `cloth.lang` remains usable
through the existing explicit forms:

```cloth
import cloth.lang.errors::ArgumentError;
import cloth.lang.errors::ArgumentError as StandardArgumentError;
import cloth.lang.errors.*;
```

Every descendant package under `cloth.lang` participates. The similarly named
package `cloth.language` does not. Other library areas, including `cloth.math`,
remain explicit imports.

## 3. Eligible declarations

Every public file type beneath `cloth.lang` is eligible, regardless of
whether it is a class, error, interface, enum, or struct. Existing
capitalization rules remain authoritative: an uppercase file type is public;
a lowercase or underscore-leading file type is private and never enters the
prelude.

The prelude exposes type names only. It does not import individual functions,
fields, constructors, enum cases, or nested declarations. Members retain
ordinary qualification through their owning file type. Prelude names do not
re-export from a consuming source file or package.

Compiler-owned primitives, `object`, arrays, `void`, `Error`,
`DivisionByZero`, allocation, error propagation, meta operations, and physical
ABI behavior remain outside `cloth.lang`. A public prelude declaration whose
local name conflicts with a compiler-owned core symbol is invalid; the compiler
must reject the library input rather than silently replace either definition.

## 4. Deterministic lookup

Unqualified names retain the existing lookup order with one added fallback:

1. lexical locals and parameters;
2. current file-class members;
3. current-package public file types;
4. explicit imports and aliases;
5. explicit wildcard imports;
6. eligible `cloth.lang` prelude types; and
7. compiler-owned core symbols.

Prelude short names must be globally unique across the complete recursive
namespace. If two public declarations at any depths share a short name, the
standard-library input is invalid and the compiler reports all conflicting
canonical identities in deterministic order.

A same-package type, explicit import, alias, or explicit wildcard binding with
the same local name wins. This low-precedence rule prevents a later compatible
prelude addition from changing the meaning of already-valid source. The
canonical library type remains reachable through an explicit alias when
another binding uses its short name.

An explicit wildcard import of a concrete package beneath `cloth.lang` is still
ordinary and nonrecursive, and retains ordinary collision diagnostics.
Explicitly importing the same prelude type is idempotent. Two distinct explicit
wildcard imports with the same local name remain ambiguous unless an exact
import or alias resolves the name.

Lookup is case-sensitive after existing portable case-collision validation.
Prelude candidates are ordered by canonical nominal identity before binding,
so filesystem order, source enumeration order, dependency declaration order,
and parallel scheduling cannot affect resolution or diagnostics.

## 5. Compiler and toolchain boundary

The compiler derives the prelude exclusively from verified declarations in the
canonical `cloth` package already present in the compilation. This works for
whole-project library source and for imported interface artifacts. The compiler
does not read `cloth-toolchain.json`, inspect `Shuttle.toml`, scan the `std`
checkout, or discover a machine-global library.

Shuttle continues to select and inject the standard library exactly as defined
by Stage 35. It does not enumerate `cloth.lang`, synthesize imports, interpret
visibility, or maintain a prelude list. No Shuttle production change is
expected for Stage 36.

Direct core-only `clothc` compilation remains valid when source uses only core
symbols. A caller that wants library types must continue to provide the
canonical `cloth` source or artifact through the explicit package boundary.
Without that input, a prelude type is unavailable and ordinary unknown-type
diagnostics apply; the compiler never searches for missing source.

The standard library itself compiles without a self-dependency. Its complete
source set supplies `cloth.lang` candidates during bootstrap, allowing other
library areas to use the same prelude rules as consumers.

## 6. Compatibility and evolution

The prelude uses existing public file declarations and canonical type
identities. Stage 36 targets unchanged artifact format **5**, compiler ABI
**5**, runtime ABI **4**, process protocol **2**, receipt schema **1**, manifest
schema **1**, and toolchain-metadata schema **1**. A discovered representation
gap requires an explicit amendment before implementation continues.

Consumer artifacts record canonical `cloth.lang` identities and the exact
standard-library version and digest through the existing dependency inventory.
There is no serialized synthetic-import list and no distinct prelude version.
Source-free consumers therefore resolve the same declarations as whole-project
compilations.

Adding a public file anywhere beneath `cloth.lang` expands the global fallback
name set and requires explicit API review and a standard-library version change.
Removing or incompatibly changing a public prelude type is a breaking library
change. Implementation-only fixes retain normal patch-version treatment. The
initial API addition advances the exact standard-library package version from
`0.1.0` to `0.2.0`. Existing artifact, capability, toolchain-metadata, receipt,
and cache inputs carry that exact version and digest; no compatibility schema
changes.

## 7. Initial `lang` API

Stage 36.3 adds exactly two public, extensible error types:

```cloth
// cloth.lang.errors.ArgumentError
error {
  ArgumentError() {}
  ArgumentError(string message): Error(message) {}
}

// cloth.lang.errors.StateError
error {
  StateError() {}
  StateError(string message): Error(message) {}
}
```

`ArgumentError` reports rejected caller input. `StateError` reports an
operation rejected because of the receiver's current state. Both retain the
compiler-owned `Error.Message` contract, can be used as application error
bases, and are available without imports. Stage 36.3 does not reinterpret
array bounds, conversion, shift, byte-operation, or non-null traps, and it does
not replace compiler-owned `DivisionByZero`.

## 8. Diagnostics

An invalid public prelude declaration is diagnosed at its source or imported
artifact location. Core-name conflicts identify both the rejected prelude name
and its compiler-owned binding. Malformed, duplicate, inaccessible, or
incompatible imported declarations remain artifact-validation errors.

Shadowing an implicit prelude name through a valid higher-priority binding is
intentional and produces no warning. Explicit import and wildcard collisions
retain their current source diagnostics. A missing standard-library
distribution remains a Shuttle toolchain error before compilation, rather than
an unknown-type cascade.

## 9. Verification

Implementation verification must cover:

- every eligible file-type kind and public/private capitalization boundary;
- exact, aliased, wildcard, implicit, recursive-package, and namespace-boundary
  behavior;
- each lookup-precedence level, idempotent explicit imports, and explicit
  wildcard ambiguity;
- core-name conflicts, absent library inputs, malformed artifacts, and
  deterministic diagnostic order;
- standard-library bootstrap without a self-edge;
- whole-project, separate-package, and source-free equivalence;
- exact library and consumer invalidation, warm reuse, failure preservation,
  and stale-output prevention;
- relocated one-job/four-job artifacts on x86-64 and wasm32;
- verified LLVM before and after optimization on both targets and native
  x86-64 execution; and
- compiler, runtime, Shuttle, standard-library, editor, documentation,
  formatting, link, sanitizer, and repository gates.

## 10. Stage plan

1. **36.1 — Prelude contract (complete).** Freeze namespace, eligibility,
   lookup precedence, compiler/toolchain ownership, compatibility, diagnostics,
   verification, and non-goals.
2. **36.2 — Prelude resolution (complete).** Implement deterministic compiler
   lookup from whole-project and imported direct `cloth.lang` declarations,
   including focused valid, precedence, collision, malformed-state, and source-
   free tests.
3. **36.3 — Initial `lang` API slice (complete, amended).** Extend prelude
   eligibility recursively beneath `cloth.lang`, add `ArgumentError` and
   `StateError` in `cloth.lang.errors`, enforce global short-name uniqueness,
   advance the paired package to `0.2.0`, and integrate native and cross-target
   package behavior.
4. **36.4 — Exit audit (complete).** Close the complete lookup, bootstrap,
   compatibility, invalidation, determinism, native/cross-target,
   documentation, and repository quality matrices.

Stage 36 is complete. On 2026-09-06, the development and sanitizer configurations
each passed all 255 CTests, including the complete compiler-backed and native
Shuttle suites. Rust 1.85, warning-denied Clippy, editor, documentation,
formatting, and repository gates also passed. The audit changed no production
behavior or compatibility version.

## 11. Non-goals

Stage 36 does not add:

- implicit visibility for all of `cloth`, recursive source wildcard imports,
  package re-exports, module declarations, or textual includes;
- a prelude opt-out directive, source-level `using` construct, import warning,
  or alternate namespace syntax;
- generics, traits, iterable/range protocols, collections, parsing, formatting,
  console input, filesystem, networking, or concurrency APIs;
- source replacements for compiler-owned core types or runtime behavior;
- registries, downloads, version solving, lockfiles, signing, publication, or
  machine-global library discovery;
- a compatibility-version transition without a separately approved invariant;
  or
- range, index, conversion, shift, byte-operation, I/O, parsing, collection, or
  concurrency error APIs before their owning operations have typed contracts.
