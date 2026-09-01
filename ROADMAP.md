# Cloth compiler roadmap

This roadmap is the authoritative order of compiler stages. `TODO.md` owns the
work items inside that order, while the owning contracts under `docs/` define
implemented behavior. Drafts under `docs/proposals/` are explicitly marked and
are not implementation claims. A backlog item is not scheduled merely because
it is documented.

Stages that cross the Shuttle boundary record their shared objective and exit
criteria here. Shuttle owns its implementation order and work ledger in the
`shuttle` repository; a coordinated stage closes only after both repositories
and their shared protocol tests pass.

## Stage discipline

Only one stage may be active. Every stage moves through `planned`, `active`,
and `complete` in that order.

A stage may become active only when:

1. its prerequisites are complete;
2. its objective, deliverables, non-goals, and exit criteria are written here;
3. its concrete work items exist in `TODO.md`;
4. any source-visible or ABI-visible design has been approved; and
5. implementation has received an explicit go-ahead.

Design approval alone does not authorize implementation. Once a stage is
active, newly proposed features go to the unscheduled backlog unless they are
required to satisfy an existing exit criterion. Changing active scope requires
updating this roadmap before code changes continue.

A stage is complete only when all scheduled work items are complete, affected
contracts are documented, invalid behavior has diagnostics, applicable
compiler boundaries have verification, development and sanitizer suites pass,
and deliberate deferrals are recorded in `TODO.md`.

## Completed baseline

| Stage | Result |
| --- | --- |
| 0.5 | C++23 compiler bootstrap, deterministic lexer, build, tests, and Google-style conventions |
| 1 | Two-pass parser and AST for implicit file classes |
| 2 | Name binding, type checking, semantic identities, and typed HIR |
| 3 | Callable control-flow analysis and verified MIR |
| 4 | Target data-layout and ABI model |
| 5 | Deterministic LLVM IR lowering |
| 6 | Native x86-64 runtime and executable pipeline |
| 7 | Initial typed output and `Hello, World!` execution |
| 8 | Path-derived packages, imports, and project discovery |
| 9 | Fixed-length arrays and checked indexing |
| 10 | Array iteration, testing contracts, printing, and stable object representation |
| 11 | `void` and callable return contracts |
| 12 | `final`, `static`, explicit nullability, and null ergonomics |
| 13 | Precise managed heap, shadow-stack roots, and non-moving collection |
| 14 | Immutable primitive UTF-8 strings and string meta queries |
| 15 | Universal managed `object`, type queries, and checked reference operations |
| 16 | Single inheritance, construction order, virtual dispatch, and `super` calls |
| 17 | Abstract, sealed, final-override, and covariant-return contracts |
| 18 | Interfaces, transitive conformance, and interface dispatch |
| 19 | Classical `for`, compound assignment, and prefix/postfix numeric updates |
| 20 | Contextual literals, lossless numeric widening, overload-directed literals, and checked explicit numeric conversions |
| 21 | Fixed-width integer operators and host-independent byte-order operations |

Stage 21 is the current completed language baseline. Coordinated toolchain Stage
22 and separate-compilation Stage 23 are complete, including their cross-tool
exit audits. No later stage is active.

## Stage 21: Integer binary representation and byte order

Status: **complete**

Objective: complete the fixed-width integer representation contract needed for
portable binary data without exposing native memory or host-dependent behavior.

Prerequisite: Stage 20.

Deliverables:

1. Define integer bitwise, complement, shift, and compound-assignment
   semantics, including result types, signed right shift, valid shift counts,
   and failure behavior.
2. Implement and verify those operators through parsing, semantic analysis,
   HIR, MIR, LLVM, diagnostics, and native tests.
3. Approve an explicit, host-independent contract for encoding and decoding
   fixed-width integers in little-endian and big-endian byte order.
4. Implement the approved byte-order surface with bounds validation and
   deterministic tests independent of the compiler host's endianness.

Non-goals:

- floating-point bit reinterpretation;
- unsafe memory views, pointers, unions, or packed structs;
- file, socket, protocol, or general serialization frameworks;
- rotations, arbitrary-precision integers, SIMD, and bitfield declarations;
- exposing “native endian” as portable source behavior.

Exit criteria:

- every Stage 21 item in `TODO.md` is complete;
- operator and byte-order contracts are present in the grammar and owning
  design documents;
- invalid counts, types, and buffer ranges have deterministic diagnostics or
  runtime failures as specified;
- MIR and applicable backend verification reject malformed representations;
- development and sanitizer suites pass.

## Stage 22: Shuttle project contract and compiler build protocol

Status: **complete**

Objective: establish Shuttle as Cloth's project and build system, replace the
compiler-owned metadata-only `cloth.toml` marker, and connect both tools through
a deterministic, versioned build protocol.

Prerequisite: Stage 21.

Deliverables:

1. Freeze Shuttle's implementation-language policy, the tool ownership and
   terminology, versioned `Shuttle.toml` schema, and initial Shuttle-to-compiler
   build request.
2. Bootstrap Shuttle's manifest handling and deterministic local dependency
   graph with cycle, duplicate, visibility, and missing-path diagnostics.
3. Give `clothc` explicit source-root and dependency inputs, remove manifest
   parsing and discovery from the compiler, and preserve identifier-based Cloth
   imports.
4. Add unit and cross-repository multi-project integration coverage and
   document both direct-compiler and Shuttle workflows.

Non-goals:

- remote retrieval, registries, semantic-version solving, lockfile generation,
  build scripts, or arbitrary command execution;
- a standard-library distribution mechanism;
- separate native compilation units, incremental compilation, or remote caches;
- embedding compiler internals as a Shuttle library.

Exit criteria:

- all Stage 22 items in both repositories are complete;
- equal project inputs produce an equal ordered compilation graph;
- malformed manifests and dependency graphs fail in Shuttle before source
  parsing;
- `clothc` can compile explicit inputs without a manifest and never reads
  `Shuttle.toml`; and
- development, sanitizer, and cross-tool integration suites pass.

The architectural ownership and migration rules are defined in
[`docs/shuttle_and_compiler.md`](docs/shuttle_and_compiler.md).
The exit audit and repeatable verification commands are recorded in
[`docs/testing.md`](docs/testing.md#stage-22-exit-audit).

## Stage 23: Shuttle-orchestrated separate compilation and linking

Status: **complete**

The [23.1 artifact proposal](docs/proposals/stage_23_artifacts.md) and companion
[process-v2 proposal](shuttle/docs/proposals/compiler_protocol_v2.md) were
approved with implementation authorization on 2026-08-31. Stage 23.2's
canonical identity, imported declaration views, and bounded artifact codec are
complete. Stage 23.3's package compile/link pipeline is also complete. Work
through Stage 23.4 completed equivalence and exit verification on 2026-09-01.

Objective: let Shuttle compile manifest-defined local packages independently
while the compiler preserves canonical Cloth type, descriptor, callable, and
artifact identity.

Prerequisite: Stage 22.

Deliverables:

1. Freeze the compiler-owned package artifact boundary and its versioning rules.
2. Preserve canonical mangling, type descriptors, interface identities, and
   runtime ownership across compilation units; provide verified imported
   declaration/ABI views and artifact serialization without persisting MIR.
3. Have Shuttle order and invoke compilation and linking of local dependency
   artifacts with deterministic duplicate and mismatch diagnostics.
4. Prove behavior with multi-package ABI, linker, and native integration tests.

Non-goals:

- incremental or distributed builds, remote caches, dynamic loading, package
  registries, cross-language FFI, or ABI stability across Cloth releases;
- optimization pipelines and debug information.

Exit criteria:

- all Stage 23 items in `TODO.md` are complete;
- separately compiled local packages behave identically to the existing
  whole-project pipeline for covered programs;
- incompatible or duplicate artifacts fail deterministically; and
- development and sanitizer suites pass.

## Beyond Stage 23

No later stage number is assigned yet. The remaining backlog must be reviewed
against a credible Cloth 1.0 release before Stage 24 is chartered. That
review—not ad hoc implementation—will choose and scope the next stage.

The following post-Stage-23 candidates are recorded without priority or order:

- wrapping and saturating conversions as explicit primitive meta operations;
- optional numeric literal suffixes;
- a general constant-folding optimizer stage;
- enums as an implicit file kind; and
- structs as an implicit file kind.

This candidate list assumes Stages 21–23 and their prerequisites are complete.
Each candidate still requires its own stage charter, dependency review,
non-goals, approved language or optimizer contract, concrete `TODO.md` items,
and explicit implementation go-ahead. Inclusion here does not activate or
reserve a stage number for any candidate.
