# Cloth compiler roadmap

This roadmap is the authoritative order of compiler stages. `TODO.md` owns the
work items inside that order. User-facing language documentation lives in the
`documentation/` submodule; compiler contracts and maintainer guidance live in
`docs/`. Drafts under `docs/proposals/` are explicitly marked and are not
implementation claims. A backlog item is not scheduled merely because it is
documented.

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
| 26 | Inline value structs with precise tracing, aggregate calls, and source-free package support |
| 26.5.1 | Explicit interface implementations using the existing `override` keyword |
| 27 | Scoped switch statements, exhaustive enum handling, and deterministic package evolution |
| 28 | Typed scalar constants, format-4 artifacts, and source-free constant values |
| 29 | Checked runtime integer arithmetic and deterministic terminal failures |
| 30 | Explicit wrapping and saturating integer conversions with verified lowering |

Stage 30 is the current completed language baseline. Coordinated toolchain Stage
22 and separate-compilation Stage 23 are complete, including their cross-tool
exit audits. Build-responsiveness Stage 24 is also complete.

Stage 26 is complete for value structs, including its approved source contract,
frontend, [aggregate ABI implementation](docs/proposals/stage_26_aggregate_abi.md),
and coordinated 26.4 exit audit. Native execution and source-free
packages introduced artifact format 3, compiler ABI 4, and runtime ABI 2. The explicit
interface-override follow-up and Stage 27 switch implementation are complete,
including the coordinated 27.4 exit audit on 2026-09-02.

Stage 28 is complete, including its separately authorized
[28.4 exit audit](docs/testing.md#stage-284-scalar-constant-exit-audit) on
2026-09-02. Typed scalar evaluation, native/source-free integration, artifact
format 4, dependency evolution, and all coordinated verification gates pass.
Stage 29 is complete following its separately authorized
[29.4 exit audit](docs/testing.md#stage-294-checked-runtime-arithmetic-exit-audit)
on 2026-09-03. Checked lowering, updates, package integration, runtime ABI 3,
and all coordinated verification gates pass.
Stage 30 is complete following its separately authorized
[30.4 exit audit](docs/testing.md#stage-304-integer-conversion-mode-exit-audit)
on 2026-09-03. The complete conversion matrix, target verification,
documentation, package determinism, and all coordinated quality gates pass.

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

Status: **complete — coordinated 26.4 exit audit passed 2026-09-02**

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
tests. The [26.4 exit audit](docs/testing.md#stage-264-struct-exit-audit) records
struct-specific serial/parallel byte equivalence, private-layout/member
invalidation, source-free privacy and inherited calls, native/forced-GC coverage,
and the complete compiler/Rust quality gates.

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

## Stage 26.5.1: Explicit interface overrides

Status: **complete — approved and verified 2026-09-02**

The [exit audit](docs/testing.md#stage-2651-explicit-interface-overrides) records
122/122 development and sanitizer CTests, all Rust gates, and VS Code support
checks with both compiler configurations.

Prerequisite: the completed Stage 26.4 audit. This is a focused contract follow-up,
not an expansion of struct semantics. Use the existing `override` keyword;
do not introduce `impl`.

Locally declared public instance functions that match an inherited class or
interface signature must use `override`, including abstract class declarations
that restate a requirement. A marker with neither contract is invalid. One
marker may satisfy several interfaces and a class override together. Inherited
implementations need no redeclaration, and interface declarations remain plain
`func`. Preserve return covariance, visibility, final sealing, and base-only
`super` behavior. Interface-only overrides introduce ordinary virtual slots;
their optional replaced-class-member identity remains absent.

Deliverables: semantic enforcement and precise diagnostics; source-free metadata
validation; migrated fixtures and regression coverage; updated owning contracts;
VS Code modifier/snippet support and tests. Existing artifact fields and physical
calling conventions suffice: no artifact/compiler/runtime revision is planned.
Exact compiler identity already prevents reuse across compiler builds.

Non-goals: new keywords, interface default bodies, explicit interface-qualified
implementations, struct conformance, editor scaffold redesign, or other backlog
features. Exit requires development/sanitizer, native/shared-tool, Rust, and
VS Code support checks plus synchronized compiler/Shuttle ledgers.

## Stage 27: Switch statements and exhaustive enum handling

Status: **complete — coordinated 27.4 exit audit passed 2026-09-02**

The [approved contract](docs/proposals/stage_27_switch.md) specifies the source,
flow, MIR, compatibility, and verification requirements. Frontend checking and
native lowering are implemented, including source-free packages. The
[exit audit](docs/testing.md#stage-274-switch-exit-audit) verifies dependency
evolution, stale-artifact rejection, failed-output preservation, and relocated
serial/parallel equivalence with both compiler configurations and shared gates.

Objective: branch over existing enum and integer values with exactly-once
selection, explicit case scopes, no implicit fallthrough, and compile-time
enum exhaustiveness across both source and artifact-only dependencies.

Prerequisites: completed enum/struct/override stages, existing callable and
constructor flow analysis, and the verified separate-compilation pipeline.

Deliverables:

1. **27.1 — Contract.** Approve syntax, selector/constant forms, grouped cases,
   scope, default/coverage policy, loop transfers, flow rules, resource limits,
   and package compatibility before implementation.
2. **27.2 — Frontend.** Implement parsing, typing, constant normalization,
   diagnostics, HIR verification, return/initialization/nullable-flow analysis,
   and keyword coordination. Reject native emission until lowering is ready.
3. **27.3 — Lowering.** Implement verified typed MIR switches, CFG/phi/GC
   integration, LLVM emission, invalid-enum traps, and source-free compilation.
4. **27.4 — Exit audit.** Verify native whole/separate execution, dependency
   evolution, deterministic artifacts, and failed-output preservation. Update
   user documentation, maintainer contracts, and editor support; pass both
   compiler configurations and the coordinated Rust/toolchain/editor gates.

Non-goals: pattern matching, guards, destructuring, ranges, labeled transfers,
fallthrough, switch expressions, other selector types, enum payloads, general
constant folding, a new local initialization policy, new targets, or unrelated
Shuttle/editor features. No ABI/artifact/protocol revision is expected; any
necessary compatibility change requires review before implementation.

Exit criteria:

- every Stage 27 item in both work ledgers is complete;
- exhaustiveness, normalized constants, scoped transfers, return paths,
  initialization, and GC behavior match the approved contract;
- source and malformed HIR/MIR fail deterministically at the proper boundary;
- source-free dependencies preserve enum coverage and constant identity, and
  edits invalidate affected consumers without replacing outputs on failure;
- whole/separate and serial/parallel builds remain equivalent; and
- development, sanitizer, native/shared-tool, Rust, and editor checks pass,
  with user documentation separated from maintainer implementation records.

## Stage 28: Compile-time scalar constants

Status: **complete — coordinated 28.4 exit audit passed 2026-09-02**

The [approved contract](docs/proposals/stage_28_scalar_constants.md) defines
eligible scalar expressions, typing, evaluation, dependency cycles, resource limits,
and the artifact transition. The user approved its concrete rules on 2026-09-02.
The separate 28.2, 28.3, and 28.4 go-aheads were received on 2026-09-02.

Objective: give every supported `static final` scalar field one verified
compile-time value shared across checking, switch labels, native emission,
and source-free packages, without introducing runtime initialization.

Prerequisites satisfied: numeric typing/checked conversions, integer operators,
static final ownership, enums, verified switches, and deterministic separate
compilation. Full aggregate constants and optimizer folding are not prerequisites.

Deliverables:

1. **28.1 — Contract (complete).** Approve scalar/operator/conversion eligibility,
   constant-context overflow and finite-float rules, short-circuiting,
   forward references/cycles, limits, compatibility, and verification gates.
2. **28.2 — Typed evaluation (complete).** Implement canonical typed scalar values,
   checked evaluation, memoized dependency resolution, diagnostics/limits,
   and the recorded unary-initializer correction. Gate new-form emission
   until every downstream boundary is ready.
3. **28.3 — Integration (complete).** Connect HIR/MIR verification, LLVM static globals,
   switch references, imported constants, and the reviewed artifact transition
   with coordinated Shuttle capability/receipt validation.
4. **28.4 — Exit audit (complete).** Verify native whole/separate/source-free behavior,
   target-independent scalar bits, malformed input/resource boundaries,
   invalidation, preserved outputs, and serial/parallel determinism. Update
   owning docs and pass compiler, Rust, shared-toolchain/native, and editor gates.

Approved compatibility transition: artifact format **4** to encode negative signed
integer constants; compiler ABI **4**, runtime ABI **2**, process protocol **2**, receipt
schema **1**, and manifest schema **1** remain unchanged. Current format/version
constants transition together in 28.3; older artifacts must be rebuilt.

Non-goals: runtime static initialization, mutable/reference static fields,
aggregate constants, enum metadata/payloads, compile-time function execution,
new keywords/literals, inline expression switch labels, local constant propagation,
runtime arithmetic-policy changes, optimizer folding, new targets, or unrelated
Shuttle/editor work.

Exit criteria:

- every Stage 28 work item in both repositories is complete;
- all accepted constant forms have the same verified type/value across checking,
  native emission, artifacts, and source-free consumers;
- evaluation preserves existing typing/conversion rules with the approved
  constant-context failure policy and deterministic resource bounds;
- the reviewed artifact change lands atomically across readers, writers, public
  capability/receipt checks, fixtures, and docs; earlier artifacts require rebuilds;
- constant/dependency edits, failures, and serial/parallel builds retain the
  existing integrity, output-preservation, and deterministic-build contracts; and
- development/sanitizer, shared native/protocol, Rust, and editor gates pass,
  with implementation claims only in the owning implemented documentation.

## Stage 29: Checked runtime integer arithmetic

Status: **complete — 29.4 exit audit passed 2026-09-03**

The [approved contract](docs/proposals/stage_29_checked_runtime_arithmetic.md)
defines checked runtime integer arithmetic before Cloth introduces optimization
or explicit wrapping/saturating operations. The user approved its concrete
runtime-failure, lowering, runtime-ABI-3, verification, and non-goal rules on
2026-09-03. The separately approved 29.2 implementation completed the direct
checked-lowering and runtime-ABI transition on the same date.

Objective: ensure ordinary integer arithmetic either produces a representable
fixed-width result or terminates through Cloth's deterministic runtime-failure
path before LLVM can execute an invalid operation.

Deliverables:

1. **29.1 — Contract (complete).** Freeze integer types/operations, exact failures,
   evaluation/store ordering, LLVM strategy, compatibility, tests, and non-goals.
2. **29.2 — Checked lowering (complete).** Add overflow/divisor guards, the runtime helper,
   verifier coverage, and the coordinated runtime-ABI transition.
3. **29.3 — Updates and integration (complete).** Prove identical behavior for
   direct, prefix/postfix, compound, native, package, and source-free execution.
4. **29.4 — Exit audit (complete).** Complete boundary/malformed/determinism tests,
   documentation, ledgers, and every compiler/Shuttle/editor quality gate.

Approved compatibility transition: runtime ABI **3**. Artifact format **4**,
compiler ABI **4**, process protocol **2**, receipt schema **1**, and manifest
schema **1** remain unchanged. Runtime-ABI-2 packages would require rebuilding.

Non-goals: float-policy changes, wrapping/saturating operations, numeric suffixes,
general folding, optimization controls, recoverable exceptions, new runtimes or
targets, and unrelated language/tooling work.

Exit requires exact runtime/constant agreement, guards before invalid LLVM
operations, exactly-once update/compound targets, coordinated compatibility and
source-free behavior, deterministic builds, and all existing verification gates.

## Stage 30: Integer conversion modes

Status: **complete — 30.4 exit audit passed on 2026-09-03**

The [approved contract](docs/proposals/stage_30_integer_conversion_modes.md)
defines `Target::wrap(value)` and `Target::sat(value)` as explicit integer-only
primitive meta conversions. The target type owns the conversion range; ordinary
`Target(value)` conversions remain checked.

Objective: provide concise, deterministic out-of-range conversion modes without
weakening Cloth's checked default or changing implicit conversion rules.

Deliverables:

1. **30.1 — Contract (complete).** Freeze syntax, signedness, aliases, constants,
   lowering, compatibility, tests, and non-goals.
2. **30.2 — Frontend and constants (complete).** Add parsing, semantic/HIR
   representation, diagnostics, constant evaluation, and verifier coverage.
3. **30.3 — Lowering and integration (complete).** Add verified MIR/LLVM
   lowering and native, separate-package, source-free, and Shuttle coverage.
4. **30.4 — Exit audit (complete).** Complete the conversion matrix,
   documentation, determinism, and all compiler/toolchain quality gates.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and manifest schema **1** remain unchanged. No new
runtime helper or Shuttle protocol behavior is planned.

Non-goals include wrapping/saturating arithmetic, floating-point modes, numeric
suffixes, user-defined conversions, optimization, exceptions, and unrelated
language or tooling work.

Exit requires exact compile-time/runtime agreement across every integer
source/target pair, verified target-independent lowering, preserved checked
conversion behavior, deterministic package builds, and all existing gates.

The 30.2 checkpoint completed on 2026-09-03. `--check` accepts typed runtime
uses, and required scalar constants evaluate with target-independent integer
semantics. At that checkpoint, ordinary compilation stopped before MIR rather
than substitute checked conversion behavior or an unverified backend path.

The 30.3 checkpoint completed on 2026-09-03. MIR retains the explicit mode and
verifies integer operands. Target-independent LLVM lowering implements wrapping
with truncation or signed/unsigned extension and saturation with comparisons and
selection; it adds no runtime helper. Direct native execution and Shuttle's
whole, separate, and source-free package paths agree, including deterministic
cross-target artifacts and affected-only invalidation.

The 30.4 exit audit completed on 2026-09-03. An independent constant oracle
covers all 81 canonical integer source/target pairs. Generated compile-time,
native, and optimized LLVM tests cover all 121 accepted type-spelling pairs,
including `int`, `uint`, and `byte`, at source and target boundaries. Checked
conversion behavior, exactly-once evaluation, every required expression
context, relocated package determinism, documentation, and all development,
sanitizer, Rust, editor, formatting, and repository gates pass.

## Stage 31: MIR optimization

Status: **complete — 31.4 exit audit passed on 2026-09-04**

The [approved contract](docs/proposals/stage_31_mir_optimization.md) defines an
always-on, target-independent MIR optimizer between verified MIR lowering and
ABI lowering. Optimization must preserve every observable source behavior,
runtime failure, diagnostic, and evaluation-order guarantee.

Objective: establish Cloth's deterministic optimization boundary and fold
provably constant scalar work without changing language correctness or exposing
backend-specific controls.

Prerequisite: Stage 30.

Deliverables:

1. **31.1 — Contract (complete).** Freeze the pipeline boundary, canonical MIR
   constants, scalar lattice, failure preservation, CFG cleanup,
   compatibility, verification, resource behavior, and non-goals.
2. **31.2 — Scalar folds (complete).** Add canonical MIR constants and
   deterministic scalar propagation/folding with verifier, backend, and
   focused coverage.
3. **31.3 — CFG and integration (complete).** Add phi propagation, constant
   branch/switch selection, canonical unreachable-block compaction,
   production-pipeline and package integration.
4. **31.4 — Exit audit (complete).** Complete equivalence, failure,
   idempotence, stress, package determinism, documentation, and all coordinated
   quality gates.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and manifest schema **1** remain unchanged. The
compiler executable digest provides normal cross-version artifact invalidation;
no optimizer setting enters public compatibility data.

Non-goals include numeric suffixes, aggregate constants, compile-time user
functions, inlining, interprocedural propagation, common-subexpression
elimination, loop/vector optimization, LLVM pass controls, public optimization
levels, debug information, exceptions, new targets, and unrelated tooling.

Exit requires exact baseline/optimized behavior, verified and idempotent MIR,
bounded deterministic analysis, x86-64/wasm32 verification, opaque Shuttle
integration, relocated package determinism, and every existing quality gate.

The 31.4 exit audit completed on 2026-09-04. Exact raw/optimized native output,
error text, status, and side effects agree; scalar boundaries, failure
preservation, structural idempotence, malformed output rejection, a 16,384-node
SSA worklist, and input-order-independent function output pass. Raw and
optimized x86-64/wasm32 LLVM pass verification before and after O2, and all
compiler, Shuttle, editor, formatting, and repository gates pass.

## Beyond Stage 31

No stage beyond 31 is assigned or active. Scheduling MIR optimization does not
schedule the rest of the backlog or freeze the Cloth 1.0 release scope.

The following candidates remain recorded without priority or order:

- optional numeric literal suffixes;
- immutable per-case enum metadata, after its constant-data prerequisites; and
- recoverable exceptions after their object/runtime ABI contract is approved.

These candidates follow completion of their prerequisites and approved stages.
Each candidate still requires its own stage charter, dependency review,
non-goals, approved language or optimizer contract, concrete `TODO.md` items,
and explicit implementation go-ahead. Inclusion here does not activate or
reserve a stage number for any candidate.
