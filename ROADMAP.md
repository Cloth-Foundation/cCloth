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
| 25 | Named value enums with public cases, scalar lowering, and separate-compilation support |

Stage 25 is the current completed language baseline. Coordinated toolchain Stage
22 and separate-compilation Stage 23 are complete, including their cross-tool
exit audits. Build-responsiveness Stage 24 is also complete.

Stage 26 is active for value structs. The approved source contract, frontend,
and [aggregate ABI implementation](docs/proposals/stage_26_aggregate_abi.md)
are complete through 26.3. Native execution and source-free packages are
supported with artifact format 3, compiler ABI 4, and runtime ABI 2. Stage 26.4
remains the coordinated equivalence and exit audit.

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

## Stage 24: Responsive and observable local builds

Status: **complete**

Objective: make Shuttle's local build loop visibly active and materially faster
without weakening exact artifact identity, deterministic output, or validation.

Prerequisite: Stage 23.

Deliverables:

1. Establish repeatable clean and unchanged-build baselines, and emit concise
   package/phase progress to standard error without contaminating compiler
   protocol or program output.
2. Remove measured cold-path overhead while retaining exact compiler, runtime,
   native-tool, source, and dependency identities.
3. Reuse validated unchanged local package artifacts across commands with
   conservative invalidation, atomic state publication, and existing writer
   exclusion.
4. Compile independent ready packages concurrently under an explicit job bound,
   while preserving deterministic artifacts and diagnostics.

Non-goals:

- per-file, per-function, or optimizer-level incremental compilation;
- a compiler daemon, watch mode, or background service;
- shared, remote, or distributed caches;
- changing Cloth source semantics, the manifest schema, or package artifact
  compatibility merely to improve speed; and
- hiding work behind unverified timestamps or trusting stale outputs.

Exit criteria:

- build progress identifies every package compile/reuse and final link, appears
  before long-running work, and leaves program standard output unchanged;
- clean builds retain exact Stage 23 behavior with materially lower measured
  overhead on the recorded small-project benchmark;
- unchanged builds perform no package compilation, while source, manifest,
  dependency, compiler, target, runtime, or native-tool changes invalidate the
  affected artifacts;
- parallel and single-job builds produce byte-identical package artifacts and
  equivalent diagnostics; and
- development, sanitizer, Rust, cross-tool, native, and responsiveness suites
  pass.

The build-output, reuse, and parallel-scheduling contracts are owned by
Shuttle's Stage 24 documents.

## Stage 25: Named value enums

Status: **complete**

Enums were selected for Stage 25 on 2026-09-02. The
[25.1 enum contract](docs/proposals/stage_25_enums.md), with always-public cases
and attached constant data deferred, received design approval and implementation
authorization on 2026-09-02.

Implementation and the coordinated exit audit completed on 2026-09-02. The
source contract is [enums](docs/enums.md), the current persistence contract is
[artifact schema v2](docs/artifact_schema_v2.md), and verification is recorded
in [the Stage 25 audit](docs/testing.md#stage-25-enum-exit-audit).

Objective: represent closed sets of named values with nominal type safety,
deterministic behavior, and equivalent whole-project and separate compilation.

Prerequisites: the Stage 21 language baseline and completed Stages 22–24.

Deliverables:

1. Approve the implicit enum file kind, case syntax with always-public cases,
   file-type visibility, value and initialization rules, permitted operations,
   printing, representation, and artifact compatibility policy.
2. Implement enum declarations, case resolution, nominal typing, initialization
   checks, and diagnostics through parser/AST, semantic analysis, and typed HIR.
3. Preserve enum identity through verified MIR, ABI layout, LLVM lowering,
   typed output, canonical identities, and compiler-owned package artifacts.
4. Verify arrays, fields, calls, imports, and cross-package execution, including
   artifact-only dependencies and whole-project/separate-build equivalence.

Non-goals:

- payload-bearing variants, enum methods, constructors, or inheritance;
- user-selected discriminants, underlying types, numeric casts, or flags;
- nested enums, structs, nullable value types, or boxing;
- `switch`, pattern matching, exhaustive-case analysis, or general reflection;
- remote dependencies, Shuttle manifest changes, or optimizer work.

Exit criteria:

- all Stage 25 items in `TODO.md` are complete;
- grammar and owning semantic, output, ABI, identity, and artifact contracts
  describe the approved behavior;
- invalid source, malformed enum IR, and malformed or incompatible artifacts
  fail deterministically without manufacturing invalid enum values;
- enums preserve their nominal identity across fields, arrays, overloads, and
  package boundaries without becoming managed class instances;
- direct and separately compiled programs produce equivalent results, and
  supported single-job/parallel builds retain deterministic artifacts; and
- development, sanitizer, affected cross-tool, and native suites pass.

## Stage 26: Value structs

Status: **active — 26.1 through 26.3 complete; 26.4 exit audit pending**

The [26.1 struct contract](docs/proposals/stage_26_structs.md), including read-only
value receivers, and the implementation start were approved on 2026-09-02.
Stage 26.3 was requested on 2026-09-02. Its
[concrete ABI/schema proposal](docs/proposals/stage_26_aggregate_abi.md) now
specifies inline layout, copy/result passing, GC maps, bounded validation, and
the coordinated artifact-3/compiler-4/runtime-2 transition. The contract and
implementation were approved on 2026-09-02. Implementation is complete, with
full-artifact golden hashes separate from the design-only review vectors.

The [struct implementation](docs/structs.md) covers frontend checking, aggregate
MIR/ABI/LLVM lowering, precise tracing, and source-free artifacts. All 121
development and 121 sanitizer CTests pass, including native and shared Shuttle
tests. The [26.3 checkpoint](docs/testing.md#stage-263-aggregate-implementation-checkpoint)
records verification; struct-specific serial/parallel equivalence and layout
invalidation remain part of the 26.4 exit audit.

Objective: introduce nominal aggregate values with explicit initialization,
predictable copying and mutation, precise tracing of contained references,
and equivalent whole-project and separate compilation.

Prerequisites: completed Stage 25, including its compiler/Shuttle exit audit.

Deliverables:

1. Approve file-based `struct { ... }`, visibility, constructors, copying,
   final/mutation boundaries, receiver semantics, equality, output, and non-goals.
2. Implement declarations, type and initialization checks, inline-layout cycle
   diagnostics, storage-location semantics, and typed HIR.
3. Freeze and implement aggregate MIR, target layout, callable ABI, LLVM
   lowering, class/array/local GC integration, and package metadata. Review
   exact format/compiler/runtime revisions and schema fixtures before changing
   compatibility. Keep Shuttle's process and manifest protocols unchanged.
4. Prove native execution, source-free imports, aggregate calls, forced-GC
   safety, serial/parallel artifact equivalence, and dependent invalidation;
   update owning contracts and complete the coordinated exit audit.

Non-goals under the approved source scope:

- inheritance, interface conformance, virtual dispatch, or `super` for structs;
- receiver-mutating instance functions, reference returns, and user copy/move
  hooks; an alternate receiver policy requires explicit contract revision;
- nullable value types, boxing, nested type declarations, or generics;
- enum-attached metadata, runtime payload variants, and pattern matching;
- aggregate static constants, dynamic static initialization, and general
  constant folding;
- custom equality/hashing, field reflection, deep cloning, or custom formatting;
- packed/native-memory representations, cross-language FFI, and raw pointers;
- moving/concurrent collection, remote dependencies, or new native targets.

Exit criteria:

- every Stage 26 item in `TODO.md` and the coordinated Shuttle ledger is complete;
- source behavior follows the approved copy, initialization, final, receiver,
  and equality contract without hidden aliasing or discarded writes;
- aggregate fields, arrays, parameters, returns, and live temporaries retain
  valid managed references across safepoints;
- invalid source and corrupted aggregate IR/layout/artifacts fail deterministically;
- direct and separate builds preserve nominal identities, physical call
  signatures, native behavior, and deterministic package artifacts; and
- development, sanitizer, native/shared-tool, and Rust quality gates pass.

## Beyond Stage 26

No later stage number is assigned. Planning structs does not schedule the rest
of the backlog or freeze the Cloth 1.0 release scope.

The following candidates remain recorded without priority or order:

- wrapping and saturating conversions as explicit primitive meta operations;
- optional numeric literal suffixes;
- a general constant-folding optimizer stage; and
- immutable per-case enum metadata, after its constant-data prerequisites.

These candidates follow completion of Stage 26 and their own prerequisites.
Each candidate still requires its own stage charter, dependency review,
non-goals, approved language or optimizer contract, concrete `TODO.md` items,
and explicit implementation go-ahead. Inclusion here does not activate or
reserve a stage number for any candidate.
