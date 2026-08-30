# Shuttle and the Cloth compiler

Shuttle is Cloth's official project, build, and package manager. Its relationship
to Cloth is the same kind of boundary that Cargo has with Rust: Shuttle owns how
a project is described and built, while the Cloth compiler owns the language and
translation of explicit inputs into verified outputs.

Shuttle is a separate tool and repository. It is not a compiler subsystem, and
the compiler does not link against Shuttle.

Shuttle is implemented in stable Rust 2024 with Rust 1.85 as its initial
minimum supported version. This choice does not enter the public build protocol
and does not require Rust on machines using released Shuttle binaries. The
owning rationale and toolchain policy are recorded in
[`shuttle/docs/implementation_language.md`](../shuttle/docs/implementation_language.md).

## Toolchain terms

- A **Cloth source package** is a namespace derived from a directory beneath a
  source root. It is the package named by Cloth import paths.
- A **Shuttle package** is one manifest-defined build and distribution unit. It
  can contain multiple Cloth source packages.
- A **Shuttle workspace** is an ordered collection of Shuttle packages that are
  built together.
- A **target** is a buildable program or library declared by a Shuttle package.

Keeping source packages distinct from Shuttle packages preserves Cloth's
path-derived imports without making filesystem paths or dependency coordinates
part of source syntax.

## Ownership boundary

| Shuttle owns | The Cloth compiler owns |
| --- | --- |
| `Shuttle.toml` and future `Shuttle.lock` files | `.co` source syntax and semantics |
| Package and workspace identity | Source-package identity beneath supplied roots |
| Dependency selection, retrieval, and graph validation | Import and visibility resolution over supplied inputs |
| Target, profile, and platform configuration | Lexing through verified HIR, MIR, ABI, and code generation |
| Deterministic build planning and compiler invocation | Compiler artifacts and artifact compatibility rules |
| Incremental state, caches, and output-directory policy | Runtime ABI and language-level diagnostics |
| Build, run, check, test, and publishing workflows | Deterministic diagnostics for explicit compiler inputs |

Shuttle must not parse Cloth source to reproduce import, type, layout, or ABI
rules. The compiler must not parse a Shuttle manifest, resolve dependency
versions, access registries, select profiles, or decide workspace policy.

## Build protocol

Shuttle validates its manifest and dependency graph before invoking the
compiler. It then supplies a deterministic, versioned build request containing
at least:

- the source root and entry sources for the target;
- the ordered dependency identities and their source roots or compiled
  artifacts;
- the target triple and compilation profile inputs;
- the requested output kind and path; and
- the versions of the build protocol and artifact format.

The compiler validates that request, resolves Cloth imports only through the
supplied roots or artifacts, and emits deterministic outputs. The process
boundary is authoritative; Shuttle must not depend on private compiler C++
headers or internal representation layouts.

The initial protocol is a direct child-process invocation. Shuttle passes
arguments as an argument vector without a command shell. Protocol version 1
defines its argument shape, path encoding, version query, exit statuses, and
diagnostic transport in
[`shuttle/docs/compiler_protocol.md`](../shuttle/docs/compiler_protocol.md).
Stage 23 will freeze the independently compiled artifact boundary and linking
behavior.

## Source imports

Shuttle does not introduce a module declaration or string-based source import.
Cloth imports remain identifier based. Shuttle maps dependency identities into
the compiler build request, and the compiler applies the language's ordinary
name-resolution and visibility rules.

A direct dependency's lowercase alias becomes the leading package component in
source imports. For example, alias `models` exposes `models::User` and
`models.data::Record`. Package names stay out of source syntax, and transitive
dependencies are not implicitly visible. The complete rule is defined in
[`shuttle/docs/manifest.md`](../shuttle/docs/manifest.md).

## Direct compiler use

`clothc` remains usable without Shuttle for compiler development, diagnostics,
and standalone source compilation. Direct mode has no manifest or dependency
manager: its roots and inputs are supplied explicitly or use the documented
single-entry fallback.

The current compiler search for a metadata-only `cloth.toml` is transitional
Stage 8 behavior. Stage 22 will replace it with an explicit compiler source-root
input. `clothc` will not search for or open `Shuttle.toml`.

## Versioning and failures

Manifest schema, build protocol, artifact format, and compiler/runtime ABI are
separate versioned contracts. A mismatch must fail before compilation or
linking rather than being guessed or silently adapted.

Diagnostics are reported by the owning layer:

- Shuttle reports manifest, workspace, dependency, profile, and build-planning
  failures.
- The compiler reports source, import, semantic, ABI, and code-generation
  failures.
- Shuttle may add build context to compiler diagnostics but must preserve their
  source locations and meaning.

This boundary is architectural. New build behavior is assigned to its owner
before it is scheduled in either repository.
