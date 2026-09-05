# Language documentation migration

## Objective

Rewrite the `documentation/` submodule as a practical language guide and reference
for cloth.dev, using the contracts in `docs/`. Organize pages by what a programmer
needs to learn or look up. Compiler implementation stages, exit audits, and
historical specifications stay in this repository's engineering documentation.

## Plan

1. Audit the existing site and source contracts. Prefer the dedicated, current
   feature contract over stale summaries; use the grammar and compiler to resolve
   syntax discrepancies. Record discrepancies here rather than propagating them.
2. Replace the welcome, installation, and first-program material. Build a learning
   path from files and values to a small multi-file program.
3. Rewrite the reference under `types/` and `language/`, then add `tooling/` for
   compiler use and Shuttle projects. Give every published directory an ordered
   `_meta.ts`, an overview, and working links. Remove obsolete feature pages.
4. Check navigation, local links, Markdown syntax, and example programs. Check valid
   examples with `clothc --check` and run representative complete programs. Report
   any site-build limitation: this submodule contains content, not the website app.

## Content map

| Source contract | Reader-facing destination |
| --- | --- |
| `language_design.md`, `grammar.md` | Welcome; getting started; language syntax and operators |
| `semantic_analysis.md` | Type overview; variables; functions; conversions |
| `numeric_conversions.md` | Integer and floating-point types; conversions and casts |
| `integer_binary_data.md` | Binary data; operators |
| `strings.md` | Strings |
| `arrays_and_indexing.md`, `array_iteration.md` | Arrays; control flow |
| `objects.md` | Object; conversions and casts; meta operations |
| `nullability.md` | Nullability; class initialization |
| `final_bindings.md`, `static_members.md` | Variables and visibility; static members |
| `void_and_callable_contracts.md` | Void; functions; entry points |
| `inheritance.md` | Classes; inheritance |
| `interfaces.md` | Interfaces |
| `enums.md` | Enums |
| `structs.md` | Structs |
| `packages_and_imports.md` | Packages and imports |
| `printing_and_object_representation.md` | Printing |
| `control_flow_and_mir.md` | Control flow and observable evaluation rules |
| `garbage_collection.md`, `native_runtime.md` | Memory and runtime failures |
| `shuttle_and_compiler.md` | Compiler and Shuttle guides |
| `canonical_identity.md`, `imported_package_views.md` | Package identity and dependency boundaries |
| `artifact_schema_v5.md`, `data_layout_and_abi.md`, `llvm_backend.md` | Tooling compatibility and target limits |
| `artifact_schema_v1.md`, `artifact_schema_v2.md` | Historical formats; no standalone learner pages |
| `testing.md`, `proposals/` | Verification and conflict resolution; no public stage/audit pages |

Installation commands are cross-checked against the root README and CMake presets.
Manifest examples are cross-checked against `shuttle/docs/manifest.md` and the
checked-in example project. These supplement, rather than replace, `docs/`.

## Resolved source discrepancies

- `structs.md` documents native execution and source-free artifacts. Older
  summaries in `language_design.md`, the root README, and the printing contract
  still describe frontend-only support. Teach the completed struct contract.
- The package guide contains `import services.api::*;` in one example, while its
  prose and `grammar.md` specify `import services.api.*;`. Use the grammar form.
- Older object and array sections describe past boundaries. Interfaces, enum and
  struct output, and aggregate reference tracing follow their dedicated contracts.
- Static members now include direct enum-case constants, as specified in
  `enums.md` and `grammar.md`.
- Artifact compatibility follows v5 / compiler ABI 5 / runtime ABI 4; process
  protocol 2 and manifest schema 1 are separate contracts.
- Shuttle searches `PATH` only when `--compiler` is omitted. An explicit compiler
  argument is a filesystem path, as confirmed in `shuttle/src/compiler.rs`.
- The grammar delegates lexical details to the lexer. Comment, decimal-literal,
  and escape rules were checked against `src/lexer/lexer.cc` and `literal.cc`.

## Route changes

Existing accurate route names remain where practical. Obsolete compound pages
are replaced with focused pages:

| Old path below `/docs/reference/` | Replacement |
| --- | --- |
| `types/any-and-void` | `types/object` and `types/void` |
| `types/arrays-and-tuples` | `types/arrays` |
| `language/classes-and-structs` | `language/classes` and `types/structs` |
| `language/interfaces-traits-enums` | `language/interfaces` and `types/enums` |
| `language/functions-and-fragments` | `language/functions` |
| `language/arrays` | `types/arrays` |
| `language/nested-classes` | `language/syntax` (current file-type boundary) |
| `language/annotations` | `language/syntax` (supported declarations) |

The website host can add redirects for these old URLs when integrating the
content. Redirect configuration is outside this content-only checkout. Do not
retain obsolete language claims to preserve a URL.

## Maintenance

Each semantic change should update its engineering contract and the corresponding
reader page together. Keep one primary page per rule, link to it from tutorials,
and distinguish supported behavior from unavailable syntax without roadmap stage
numbers. Store all content pages as `.md`, with `index.md` for section overviews;
retain `_meta.ts` for navigation. Use `cloth` fences for examples and `/docs/...`
for site links. Public
pages must be useful without access to the compiler repository.

## Completed first migration

The audit and rewrite are complete: 38 content pages and six `_meta.ts` files
cover getting started, types, language rules, and tooling. Eight obsolete pages
were removed in favor of the focused routes above. The compiler contracts were
not edited as part of the website rewrite.

Validation against the local toolchain:

- All 67 Cloth code blocks passed `build/dev/clothc.exe --check`, with statement
  fragments wrapped in functions and referenced types supplied as separate files.
- Eight complete native programs produced byte-for-byte expected output: Hello
  World, language basics, the multi-file tutorial, struct copies, enum output,
  inherited dispatch, interface dispatch, and binary data.
- The tutorial manifest passed Shuttle check, build, and run, including compiler
  discovery through `PATH` and explicit compiler-path selection.
- Every local site link resolves; each published content directory has an index
  and `_meta.ts` listing exactly its pages and child sections.
- Code fences, titles, and table structure were checked. `git diff --check` passed.
- All 38 content pages use `.md`. The 37 former `.mdx` pages were renamed without
  changing their contents; extensionless site links and `_meta.ts` keys retain
  their routes.

Full website rendering remains an integration check: this submodule has no
website application, package manifest, Markdown renderer, or host build command.
Structural checks do not replace that build. Optional old-URL redirects likewise
belong to the website host. No content has been committed or published.
