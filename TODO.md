# Cloth work ledger

`ROADMAP.md` defines stage order and scope. This file tracks the concrete work
inside scheduled stages and preserves accepted but unscheduled gaps. Implemented
user behavior belongs in `documentation/`; compiler contracts and maintainer
guidance belong in `docs/`, rather than in a growing history of checked boxes.

Rules:

- only work under the active stage may be implemented as a feature;
- a planned stage is not active until its design and implementation start are
  explicitly approved;
- new ideas enter the unscheduled backlog unless the roadmap is changed first;
- completing an item requires implementation, verification, tests, and updates
  to its owning contract; and
- moving or splitting an item must preserve its prerequisites and non-goals.

## Stage status

Stage 22 is complete, including the cross-tool exit audit in `docs/testing.md`.
Stage 23 is also complete, including its coordinated development and sanitizer
exit audit. Stage 24 is complete under the responsive-build charter in
`ROADMAP.md`. The ownership boundary remains recorded in
`docs/shuttle_and_compiler.md`.

Stage 25 is complete for named value enums. Its
[25.1 contract](docs/proposals/stage_25_enums.md) and implementation start were
approved on 2026-09-02, followed by the coordinated exit audit in `docs/testing.md`.
Attached constant data and runtime payloads remain deferred.

Stage 26 is complete for value structs. The
[26.1 source contract](docs/proposals/stage_26_structs.md), including read-only
value receivers, and 26.2 implementation were approved on 2026-09-02.
The frontend and [26.3 aggregate implementation](docs/proposals/stage_26_aggregate_abi.md)
are complete, including native execution and source-free packages. Artifact
format 3, compiler ABI 4, and runtime ABI 2 were introduced at that checkpoint.
The [26.4 exit audit](docs/testing.md#stage-264-struct-exit-audit) passed on
2026-09-02. Stage 26.5.1 is complete for the approved explicit interface-override
contract; its exit audit is recorded in `docs/testing.md`. Unrelated ideas
remain in the backlog.

Stage 27 is complete. Its [switch contract](docs/proposals/stage_27_switch.md)
and implementation through 27.4 were approved on 2026-09-02. Frontend, native
lowering, and the coordinated [exit audit](docs/testing.md#stage-274-switch-exit-audit)
are complete.

Stage 28 is complete. The user approved the
[28.1 scalar-constant contract](docs/proposals/stage_28_scalar_constants.md) on
2026-09-02, including its concrete evaluation rules and format-4 transition.
The separately authorized 28.2 evaluation, 28.3 integration, and
[28.4 exit audit](docs/testing.md#stage-284-scalar-constant-exit-audit) are
complete. Stage 29 is also complete following its separately authorized
[29.4 exit audit](docs/testing.md#stage-294-checked-runtime-arithmetic-exit-audit)
on 2026-09-03. Stage 30.1 is complete for the approved integer conversion-mode
contract. The separately authorized 30.2 frontend and constant checkpoint is
also complete. The separately authorized 30.3 lowering and integration
checkpoint is complete. Stage 30 is complete following its separately
authorized 30.4 exit audit on 2026-09-03. Stage 31.1 is complete for the
approved MIR optimization contract. The separately authorized 31.2 scalar-fold
and 31.3 CFG/integration checkpoints are also complete. Stage 31 is complete
following its separately authorized 31.4 exit audit on 2026-09-04. Stage 32 is
complete following its separately authorized 32.4 exit audit on 2026-09-04.
Stage 33 is complete following its separately authorized 33.4 numeric-notation
exit audit on 2026-09-04. Stage 34 is complete following its separately
authorized 34.4 typed-error exit audit on 2026-09-05.
Stage 35 is complete following its separately authorized 35.4 exit audit on
2026-09-05.
Stage 36 is complete following its separately authorized 36.4 exit audit on
2026-09-06.
Stage 37 is complete following its separately authorized 37.4 exit audit on
2026-09-06.
Stage 38 is complete following its separately authorized 38.4 exit audit on
2026-09-06.

## Scheduled work

### Stage 21: Integer binary representation and byte order

- [x] **21.1 — Integer operator contract.** Freeze valid operand types, result
  types, precedence, signed complement, signed right shift, shift-count bounds,
  compound assignment, constant behavior, and diagnostics for `&`, `|`, `^`,
  `~`, `<<`, and `>>`.
- [x] **21.2 — Integer operator implementation.** Carry the approved contract
  through parser/AST, semantic analysis, HIR, MIR verification, LLVM lowering,
  invalid-program coverage, and native execution tests.
- [x] **21.3 — Byte-order contract.** Approve the source surface and semantics
  for explicit little-endian and big-endian encoding and decoding of fixed-width
  integers. Specify byte-array size, offset, bounds, signedness, evaluation
  order, and failure behavior without exposing host-native memory.
- [x] **21.4 — Byte-order implementation.** Implement the approved surface,
  retain the operation explicitly through verified compiler representations,
  and test identical byte sequences independent of target layout metadata.
- [x] Complete the Stage 21 exit audit in `ROADMAP.md`, including development
  and sanitizer suites and recording every deliberate deferral below.

### Stage 22: Shuttle project contract and compiler build protocol

- [x] **22.1 — Manifest and build-protocol contract.** Freeze Shuttle's Rust
  2024 implementation policy, `Shuttle.toml`, package/workspace terminology,
  dependency namespace mapping, the versioned Shuttle-to-compiler request,
  migration from `cloth.toml`, and diagnostics ownership. The approved contracts
  live under `shuttle/docs/` and `docs/shuttle_and_compiler.md`.
- [x] **22.2 — Shuttle bootstrap.** Establish the Rust application, production
  CLI and diagnostics boundaries, manifest model and parser, deterministic
  validation, formatting, linting, and unit-test harness.
- [x] **22.3 — Local graph and compiler integration.** Resolve local
  dependencies deterministically in Shuttle; add the approved package graph,
  source-root, dependency, entry, target, and output inputs to `clothc`; remove
  compiler manifest discovery; and preserve standalone compilation and
  identifier-based imports.
- [x] **22.4 — Cross-tool verification.** Add configuration unit tests,
  multi-project fixtures spanning both repositories, direct and Shuttle build
  documentation, and the Stage 22 development/sanitizer exit audit.

### Stage 23: Shuttle-orchestrated separate compilation and linking

- [x] **23.1 — Artifact contract.** Define the versioned package artifact and
  the semantic/ABI, native payload, and dependency metadata it owns. Approve
  the [artifact proposal](docs/proposals/stage_23_artifacts.md) and
  [process-v2 proposal](shuttle/docs/proposals/compiler_protocol_v2.md), including
  check-only artifacts, compatibility, validation, and deliberate non-goals.
- [x] **23.2 — Canonical identity.** Preserve type descriptors, interface
  identities, mangled callables, constructor initializer linkage, and runtime
  ownership across artifacts. Implement imported declaration/ABI views and
  bounded artifact serialization/verification with canonical schema fixtures;
  do not serialize MIR or dependency bodies.

  - [x] Retain exact package versions and implement canonical type/member
    identities with fixed encoding/hash fixtures and order/alias/path tests.
  - [x] Give descriptors canonical ownership, expose accessible constructor
    initializers, and add a package-scoped LLVM definition/declaration boundary.
  - [x] Build owned, verified imported declaration/ABI views without synthetic
    source ASTs or dependency bodies.
  - [x] Freeze artifact record-schema fixtures and implement bounded canonical
    serialization, integrity/compatibility validation, and malformed-input tests.

  Stage 23.2 is documented in [canonical identity](docs/canonical_identity.md),
  [imported package views](docs/imported_package_views.md), and the
  [artifact schema](docs/artifact_schema_v1.md). Protocol-v2 compilation,
  dependency closure, and linking are connected by Stage 23.3.

- [x] **23.3 — Link pipeline.** Make Shuttle order and invoke deterministic
  compilation and linking while the compiler diagnoses duplicate, missing, or
  incompatible artifact definitions.
- [x] **23.4 — Equivalence verification.** Compare separate and whole-project
  compilation in ABI, linker, invalid-input, and native execution tests, then
  complete the Stage 23 development/sanitizer exit audit.

### Stage 24: Responsive and observable local builds

- [x] **24.1 — Baseline and progress contract.** Record clean-build phase
  timings and define stable package, link, completion, and run progress on
  standard error without changing compiler diagnostics or program streams.
- [x] **24.2 — Cold-path efficiency.** Optimize the measured exact-identity and
  process overhead while preserving Stage 23 compatibility and validation.
- [x] **24.3 — Validated local reuse.** Persist conservative Shuttle-owned
  package input state, reuse only integrity- and compatibility-validated
  artifacts, and invalidate affected dependents precisely.
- [x] **24.4 — Deterministic parallel scheduling.** Execute independent ready
  packages under a bounded job policy and prove single-job/parallel output and
  diagnostic equivalence.
- [x] Complete the Stage 24 development, sanitizer, Rust, cross-tool, native,
  cold-build, and unchanged-build exit audit.

### Stage 25: Named value enums

- [x] **25.1 — Enum contract.** Approve the implicit file kind, case syntax,
  always-public cases, file-type visibility and imports, nominal value semantics,
  initialization, equality and conversion policy, typed output and meta queries,
  representation, artifact compatibility, diagnostics, and deliberate non-goals.
  The approved contract is in [the enum proposal](docs/proposals/stage_25_enums.md).
- [x] **25.2 — Declarations and type checking.** Implement the approved syntax
  in the two-pass parser and AST; register enum types and cases before bodies;
  bind qualified cases with public access independent of spelling; enforce
  nominal assignments, overloads, initialization, type visibility, and invalid
  operations; preserve identity in typed HIR. Add parser, semantic, flow, HIR,
  and invalid-program coverage.
- [x] **25.3 — Lowering and package integration.** Implement verified enum
  constants and operations in MIR, scalar ABI storage/calls/arrays, LLVM and
  typed output, canonical identities, and bounded imported artifact records.
  Freeze the reviewed format/ABI revisions and record fixtures before changing
  serialization. Verify malformed values, type confusion, metadata ownership,
  compatibility rejection, and non-reference GC layout.
- [x] **25.4 — Equivalence and exit audit.** Test fields, constructors, static
  constants, parameters, returns, overloads, arrays and iteration, imports,
  output, and dependencies with unavailable sources. Compare direct and
  separately compiled execution, serial and parallel artifacts, and Shuttle
  reuse/invalidation after enum changes. Update owning contracts and run the
  development, sanitizer, affected cross-tool, and native suites.

### Stage 26: Value structs

- [x] **26.1 — Struct contract.** Approve implicit file identity and syntax,
  constructor visibility, explicit initialization, nominal copying, writable
  locations, final propagation, instance receiver policy, fieldwise equality,
  typed output/meta queries, and deliberate non-goals.

  - [x] Draft the source contract and identify aggregate ABI, GC, and package
    prerequisites in [the struct proposal](docs/proposals/stage_26_structs.md).
  - [x] Approve read-only value receivers and activate frontend implementation
    on 2026-09-02. Keep in-place receiver mutation deferred.

- [x] **26.2 — Frontend and value checking.** Add the struct envelope and
  symbols to the two-pass parser; enforce ordinary capitalization visibility,
  exact type identity, constructors, required field/local initialization,
  read-only/writable locations, temporary mutation diagnostics, final paths,
  permitted operations, and inline-layout cycle detection. Retain explicit
  aggregate values and storage paths in typed HIR with negative verifier tests.
  Completed on 2026-09-02 with 100/100 development and sanitizer CTests, including
  the existing cross-tool/native suites. At that checkpoint, `--check` validated
  without lowering and native/ABI/artifact support remained gated until 26.3.
  See [structs](docs/structs.md)
  and the [verification checkpoint](docs/testing.md#stage-262-struct-frontend-checkpoint).
- [x] **26.3 — Aggregate lowering and package integration.** Preserve copy,
  receiver, and result semantics through MIR, ABI, LLVM, GC, and artifacts.

  - [x] Draft exact aggregate layouts, callable passing modes, array reference-
    offset metadata, validation limits, ABI/schema revisions, and canonical
    record/layout review vectors in [the 26.3 proposal](docs/proposals/stage_26_aggregate_abi.md).
  - [x] Obtain approval and freeze that contract before implementing the
    aggregate ABI/schema boundaries. Review vectors are not `.cpa` artifacts;
    freeze full golden bytes/digests with the reviewed format-3 reader/writer.
  - [x] Implement nested inline layouts, field/index mutation with evaluate-once
    behavior, aggregate calls/results, equality, and typed output/meta queries.
  - [x] Trace embedded references in classes/arrays and root live aggregate
    locals, parameters, construction state, temporaries, and return paths.
    Protect storage owners across safepoints; reject malformed root maps.
  - [x] Preserve source-free declaration/layout/callable identity and validation;
    coordinate version requirements and opaque receipts with Shuttle without
    changing process protocol or manifest schema.

  Completed on 2026-09-02 with 121/121 development and sanitizer CTests,
  source-free native/wasm32 fixtures, full-artifact golden hashes for both target
  layouts, and all ordinary Rust checks. See the
  [implementation checkpoint](docs/testing.md#stage-263-aggregate-implementation-checkpoint).

- [x] **26.4 — Equivalence and exit audit.** Cover shallow copying, final and
  reference boundaries, nested writes with side effects, constructor flow,
  equality/NaN, arrays and iteration, overloads, class/interface calls carrying
  struct values, output, imported private layouts, and forced-GC survival.
  Verify malformed IR/ABI/artifacts, source-free dependencies, whole-project
  versus separate execution, serial/parallel bytes, and layout-change
  invalidation. Update owning documents and pass development, sanitizer,
  native/shared-tool, and Rust gates before marking the stage complete.

  - [x] Audit native value semantics, overloads, inherited/interface calls,
    constructor flow, final/reference boundaries, and malformed aggregate IR.
  - [x] Prove relocated serial/parallel struct artifacts are byte-identical;
    verify private-layout/member edits invalidate consumers while unrelated
    packages remain reusable, including source-free rejection of private access.
  - [x] Run the complete development, sanitizer, native/shared-tool, and Rust
    matrix; update both repositories' contracts and close the coordinated audit.

  Completed on 2026-09-02: 121/121 development and sanitizer CTests, including
  22 shared protocol and 12 native Shuttle cases, plus all 43 ordinary Rust
  tests and quality gates. The audit also corrected aggregate resource-limit
  diagnostic classification and bounded decoded descriptor maps. No syntax or
  compatibility revision changed. See the [exit audit](docs/testing.md#stage-264-struct-exit-audit).

### Stage 26.5.1: Explicit interface overrides

- [x] Require `override` on locally declared interface implementations and
  abstract class restatements; reject unmatched markers while preserving
  inherited implementations, final/covariant overrides, and base-only `super`.
- [x] Verify the explicit marker and replaced-class-member identity in imported
  packages without changing record shapes or physical ABI; migrate fixtures and
  test source-free, multiple-interface, inherited, and invalid contracts.
- [x] Update documentation and VS Code highlighting/snippets with regression
  tests; pass development/sanitizer, shared/native, Rust, and editor checks.

Completed on 2026-09-02: 122/122 development and sanitizer CTests, all 43
ordinary Rust tests and quality gates, and three VS Code checks with each
compiler. The covariance regression also removed source-order dependence from
class override validation. Existing ABI and artifact versions are unchanged.

### Stage 27: Switch statements and exhaustive enum handling

- [x] **27.1 — Contract.** Approve the source/flow/lowering boundaries and
  authorize implementation of the [switch proposal](docs/proposals/stage_27_switch.md).

  - [x] Draft selector and label types, grouped case blocks, exhaustiveness,
    default behavior, transfers, initialization/narrowing, MIR/GC integration,
    source-free dependencies, compatibility, resource limits, and non-goals.
  - [x] Record stage order and coordinated Shuttle work without claiming the
    feature in user documentation or modifying compiler behavior.
  - [x] Record user approval of the concrete contract and 27.2 implementation
    on 2026-09-02.

- [x] **27.2 — Frontend.** Add keyword/parser/AST support and recovery; bind and
  normalize labels; reject duplicate, incompatible, nonconstant, and missing
  cases; enforce arm scope and transfer targets. Extend typed HIR verification,
  return/required-field/final/nullable-flow analysis, and negative tests.
  Synchronize compiler/Shuttle keyword restrictions and editor highlighting.
  Gate native/IR emission explicitly until 27.3 is ready.

  Completed 2026-09-02: 127/127 development and sanitizer CTests, 43 ordinary
  Rust tests plus formatting/Clippy/Rust 1.85 checks, and six editor checks with
  each compiler. See the [frontend audit](docs/testing.md#stage-272-switch-frontend).

- [x] **27.3 — Lowering.** Add typed MIR switch terminators and full verifier,
  CFG/phi/constructor-dataflow/GC-liveness integration. Emit LLVM switches and
  invalid-enum guards; test native behavior and imported enum/static constants.
  Audit compatibility before changing any persistent format or physical ABI.

  Completed 2026-09-02: 139/139 development and sanitizer CTests, including
  forced GC, mixed phis, invalid-tag traps, maximum-size LLVM emission, and
  source-free native packages. Rust and editor gates pass. Artifact 3, compiler
  ABI 4, runtime ABI 2, and process/receipt/manifest versions are unchanged.
  See the [lowering audit](docs/testing.md#stage-273-switch-lowering).

- [x] **27.4 — Exit audit.** Cover selector side effects, scoped nested
  transfers, exhaustive returns, initialization, joins, forced GC, malformed
  representations, and case limits. Prove source-free case/constant edits,
  dependency invalidation, failed-output preservation, whole/separate behavior,
  and serial/parallel equivalence. Update `documentation/`, owning `docs/`,
  and VS Code snippets/tests; pass development/sanitizer and all shared gates.

  Completed 2026-09-02: 141/141 development and sanitizer CTests, including
  24 shared protocol and 16 native Shuttle tests; all 43 ordinary Rust tests,
  formatting/Clippy/Rust 1.85 checks, and six editor checks with each compiler.
  Enum/constant evolution, stale-consumer rejection, preserved outputs, and
  relocated serial/parallel equivalence pass. No compatibility version changed.

### Stage 28: Compile-time scalar constants

- [x] **28.1 — Contract.** Approve the
  [scalar-constant proposal](docs/proposals/stage_28_scalar_constants.md),
  including its artifact-format review, before activating implementation.

  - [x] Draft supported scalars/operators/conversions, checked constant-context
    arithmetic, finite IEEE evaluation, typing, short-circuiting, forward
    references/cycles, visibility, canonical values, and deterministic limits.
  - [x] Review downstream boundaries: format 3 cannot encode negative signed
    constants. Propose format 4 without changing physical ABI/runtime/protocol
    versions; record migration, source-free tests, and coordinated Shuttle work.
  - [x] Schedule the stage in both repositories while preserving the implemented
    user reference and current compiler/tool versions.
  - [x] Record user approval of the concrete source/evaluation/resource and
    artifact-format contract on 2026-09-02.

- [x] **28.2 — Typed evaluation.** Implement eligibility and ordinary typing,
  a shared canonical scalar evaluator, constant-reference resolution, forward
  declarations, deterministic cycle/failure handling, and resource limits.

  - [x] Obtain explicit 28.2 implementation go-ahead before activating the stage
    (received 2026-09-02).
  - [x] Retain unary-literal initializer values, including
    `static final int8 Minimum = int8(-128);`, for frontend checking and named
    switch labels. Output consumers remain explicitly gated until 28.3.
  - [x] Test checked integer arithmetic, division/remainder/shifts, literal versus
    typed conversions, finite IEEE rounding/underflow/signed-zero vectors,
    short-circuit eligibility, privacy, source order, cycles, and bounded graphs.
  - [x] Keep new-form emission explicitly gated until 28.3; never silently
    discard an initializer or synthesize a fallback value.

  Completed 2026-09-02. Per-package parser budgets and iterative preflight stop
  oversized/ineligible trees before recursive typing; canonical memoized graph
  evaluation handles the full declaration limit. New forms pass direct `--check`
  and remain rejected by emission/package paths without replacing completed
  outputs. See the [checkpoint verification](docs/testing.md#stage-282-typed-constant-checkpoint).

- [x] **28.3 — Integration.** Make verified scalar values authoritative in
  HIR/MIR, LLVM static globals, imported declarations, and existing switch-label
  references. Test malformed values and ensure no startup code or GC storage.

  - [x] Implement the approved format-4 integer encoding with full signed range;
    coordinate readers/writers, capabilities/receipts, Shuttle validation,
    golden fixtures, diagnostics, and schema docs. Reject prior formats.
  - [x] Verify private/aliased/cross-package constant chains and integer/enum
    labels without dependency sources. Preserve Stage 27 label eligibility.
  - [x] Remove the temporary gate only after whole/separate/source-free
    checking and native output agree for every supported scalar form.

  Completed 2026-09-02. Static MIR contains typed data rather than executable
  initializers; HIR independently checks claims and the dependency graph.
  Format-4 readers/writers preserve exact scalar bits, and Shuttle requires
  format 4 without interpreting metadata. Both 148-test compiler configurations,
  shared protocol/native suites, Rust, and editor gates pass. See the
  [checkpoint record](docs/testing.md#stage-283-constant-integration-checkpoint).

- [x] **28.4 — Exit audit.** Complete boundary, malformed-artifact/model,
  dependency-evolution, stale-link, failed-output, and relocated serial/parallel
  tests. Verify exact scalar bits on x86-64/wasm32 and native behavior on x86-64.
  Pass development/sanitizer, Rust, shared protocol/native, and editor gates;
  update `documentation/`, owning maintainer contracts, and both stage ledgers.

  Completed 2026-09-02 after the separate go-ahead. Fixed nested-sign overflow
  cancellation and invalid LLVM decimal emission for valid float32 literals;
  hardened retained HIR literal/group/unary checks. Added width/resource/model,
  re-signed artifact, cross-target exact-bit, computed dependency evolution,
  source-free failure, stale-link, and relocated serial/parallel regressions.
  Both 148-test compiler configurations, all 27 shared protocol and 20 native
  tests per compiler, 43 ordinary Rust tests, Rust quality/MSRV checks, and six
  editor tests per compiler pass. See the
  [exit record](docs/testing.md#stage-284-scalar-constant-exit-audit).

### Stage 29: Checked runtime integer arithmetic

- [x] **29.1 — Contract.** Review and approve the
  [runtime arithmetic proposal](docs/proposals/stage_29_checked_runtime_arithmetic.md),
  including exact operation/failure rules, evaluation ordering, LLVM guards,
  runtime ABI 3, source-free compatibility, test gates, and non-goals.

  Approved 2026-09-03. This records the checked default, exact terminal failure
  messages, exactly-once/store ordering, LLVM guards, the runtime ABI 3
  transition, test gates, and explicit non-goals. It also records division by
  zero for a future constructed-error model without activating that model.

- [x] **29.2 — Checked lowering.** After a separate implementation go-ahead,
  implement signed/unsigned overflow intrinsics, pre-division/remainder guards,
  unary negation, the runtime helper/messages, verifier hardening, and the
  coordinated runtime-ABI-3 compiler/Shuttle transition.

  Completed 2026-09-03. Direct integer expressions now use signed/unsigned LLVM
  overflow intrinsics and ordered division/remainder guards at widths 8 through
  64. One runtime helper owns the three exact failure messages; signed-minimum
  literal formation remains valid while runtime negation is checked. MIR type
  hardening, runtime/unit/native failure tests, clean-output enforcement,
  runtime-ABI-2 rejection, format-4 goldens, and the opaque Shuttle boundary are
  covered without changing public capabilities, receipts, or protocol schemas.

- [x] **29.3 — Updates and integration.** Apply and verify the same checks for
  prefix/postfix updates and arithmetic compound assignments while evaluating
  every target and operand exactly once. Cover LLVM optimization, native,
  whole/separate/source-free packages, compatibility rejection, invalidation,
  output preservation, and stale-run prevention.

  Completed 2026-09-03. MIR/LLVM tests freeze prefix/postfix result values,
  left-to-right target/RHS evaluation, and guard-before-store behavior for
  locals, members, arrays, and projected imported-struct storage. Native tests
  cover successful updates and all arithmetic compounds plus update/compound
  overflow and zero-divisor failures. Optimized x86-64/wasm32 IR verifies, and
  the public Shuttle boundary proves whole/separate/source-free equivalence,
  affected-only invalidation, output preservation, no stale execution, and
  serial/parallel artifact determinism.

- [x] **29.4 — Exit audit.** Complete all widths/endpoints and malformed-model
  coverage, deterministic relocated builds, user/maintainer documentation, and
  both compiler configurations plus Rust/shared Shuttle/editor quality gates.

  Completed 2026-09-03. Native execution covers every canonical integer width
  at zero, adjacent, minimum, and maximum endpoints. LLVM and malformed HIR/MIR
  tests freeze checked signed/unsigned lowering while excluding float, string,
  and bitwise operations. Exact runtime failures, compile-time/runtime agreement,
  relocated package determinism, both 186-test compiler configurations, all Rust,
  shared Shuttle, editor, format, and repository gates pass.

### Stage 30: Integer conversion modes

- [x] **30.1 — Contract.** Approve
  [integer conversion modes](docs/proposals/stage_30_integer_conversion_modes.md)
  with `Target::wrap(value)` and `Target::sat(value)`, exact signedness and alias
  rules, constant/runtime agreement, lowering, compatibility, tests, and
  non-goals.

  Approved 2026-09-03. Both names are contextual primitive meta operations, not
  keywords. The argument is evaluated once without target contextual typing;
  `wrap` uses mathematical modulo and target two's-complement interpretation,
  while `sat` clamps to the target range. All compatibility versions remain
  unchanged.

- [x] **30.2 — Frontend and constants.** After a separate go-ahead, implement
  parser/AST, semantic and HIR representation, exact diagnostics, required
  constant evaluation, malformed-model rejection, and focused tests.

  Completed 2026-09-03. Primitive-target parsing keeps `wrap` and `sat`
  contextual, semantic analysis preserves the argument's independent type, HIR
  carries the explicit mode, and required constants use exact signed/unsigned
  conversion rules through 64-bit boundaries. Invalid syntax, types, modes, and
  malformed HIR are rejected. Runtime uses pass frontend validation and stop
  with a stable diagnostic before the 30.3 MIR boundary. Both 188-test compiler
  configurations and all Rust, shared Shuttle, editor, formatting, and
  repository gates pass.

- [x] **30.3 — Lowering and integration.** After a separate go-ahead, implement
  verified MIR/LLVM lowering without a runtime helper and cover native,
  whole/separate/source-free packages, invalidation, and output preservation.

  Completed 2026-09-03. MIR preserves and verifies each explicit conversion
  mode. LLVM implements wrapping and saturation with target-independent integer
  operations and no runtime helper. Direct native tests cover signed/unsigned,
  narrowing, widening, boundaries, constants, and exactly-once evaluation.
  Shuttle verifies whole, separate, and source-free equivalence, affected-only
  invalidation, unrelated reuse, failure preservation, and deterministic x86-64
  and wasm32 artifacts. Both compiler configurations pass all 194 CTests; the
  shared Shuttle matrix contains 29 protocol/toolchain and 24 native cases, all
  43 ordinary Rust tests pass, and the Rust 1.85, editor, formatting, Clippy, and
  repository gates are green.

- [x] **30.4 — Exit audit.** Complete every integer source/target and boundary
  pair, relocated serial/parallel determinism, user/maintainer documentation,
  and both compiler configurations plus Rust/shared Shuttle/editor gates.

  Completed 2026-09-03. An independent constant oracle covers all 81 canonical
  source/target pairs. Generated constant/runtime and x86-64/wasm32 LLVM tests
  cover all 121 accepted spelling pairs, including `int`, `uint`, and `byte`,
  across source extrema and every representable target minimum, maximum, zero,
  adjacent, below-range, and above-range value. Runtime coverage includes
  locals, fields, arguments, returns, static constants, and exactly-once
  evaluation; checked conversion traps remain unchanged. Relocated
  serial/parallel, whole, separate, and source-free package paths agree. Both
  compiler configurations pass all 200 CTests, including 29 shared
  protocol/toolchain and 24 native Shuttle cases; all 43 ordinary Rust tests,
  Rust 1.85, editor, formatting, warning-denied Clippy, and repository gates
  pass. Stage 30 is complete with every compatibility version unchanged.

### Stage 31: MIR optimization

- [x] **31.1 — Contract.** Approve
  [MIR optimization](docs/proposals/stage_31_mir_optimization.md) with an
  always-on post-verification/pre-ABI boundary, canonical typed scalar
  constants, exact semantic preservation, deterministic folding and CFG rules,
  unchanged compatibility versions, resource bounds, tests, and non-goals.

  Approved 2026-09-03. Optimization consumes verified MIR, keeps runtime
  failures at runtime, introduces no public switch, and verifies its result
  before ABI lowering. Folded values use exact typed bits instead of synthesized
  source lexemes. The separately authorized 31.2 implementation is complete.

- [x] **31.2 — Scalar folds.** Add the canonical MIR scalar-constant
  instruction, verifier/printer/LLVM support, deterministic scalar lattice,
  successful folding of the approved pure operations, unchanged failure paths,
  imported constant propagation, and focused malformed-model tests.

  Completed 2026-09-03. The bounded per-body worklist reuses the Stage 28-30
  scalar evaluator and folds literals, verified static constants, unary and
  binary scalar operations, numeric conversions, `wrap`, and `sat`. Invalid or
  failing evaluations retain their original runtime MIR without diagnostics.
  Canonical constants verify exact types/bits, print deterministically, and
  lower to LLVM with exact floating bitcasts. Development and sanitizer builds
  each pass all 201 CTests. Production-pipeline and CFG integration were
  completed by the separately authorized 31.3 checkpoint.

- [x] **31.3 — CFG and integration.** Add equal-constant phi propagation,
  constant branch/switch selection, canonical block/value compaction,
  fixed-point idempotence, default-pipeline integration, and whole,
  separate-package, source-free, invalidation, and failure-preservation tests.

  Completed 2026-09-03. Executable-edge propagation resolves equal reachable
  phis and constant branch/switch successors. Stable compaction removes
  unreachable blocks and single-incoming phis, then rebuilds every block/value
  reference and count in source order. The production pipeline verifies raw
  MIR, optimizes it, verifies the result, and only then lowers ABI. Whole and
  source-free package compilations produce the same optimized app body;
  existing Shuttle invalidation, failure-preservation, serial/parallel, and
  native package paths pass with both compiler configurations. Development and
  sanitizer builds each pass all 201 CTests. Compatibility versions remain
  unchanged.

- [x] **31.4 — Exit audit.** Complete baseline/optimized equivalence,
  failure-text/status, scalar boundary, side-effect, stress, malformed-model,
  x86-64/wasm32, relocated determinism, documentation, and both compiler
  configurations plus Rust/shared Shuttle/editor/repository gates.

  Completed 2026-09-04. Raw and optimized native programs have identical
  output, error text, status, and side-effect counts, including preserved
  divide-by-zero failure. Boundary/operator, call/return, loop/phi,
  branch/switch, malformed post-pass, structural idempotence, stable
  input-order, and 16,384-node worklist cases pass. Raw and optimized x86-64
  and wasm32 LLVM verify before and after O2. Development and sanitizer builds
  each pass all 215 CTests, including 29 shared protocol/toolchain and 24 native
  Shuttle cases. All 43 ordinary Rust tests, Rust 1.85, Clippy, Rust and C++
  formatting, six editor tests per compiler, documentation, and repository
  gates pass. Compatibility versions remain unchanged. No Stage 32 work was
  part of this completed audit.

### Stage 32: Typed numeric literals

- [x] **32.1 — Contract.** Approve
  [typed numeric literals](docs/proposals/stage_32_typed_numeric_literals.md)
  with canonical lowercase width suffixes, exact initial typing, unchanged
  unsuffixed behavior, range and recovery rules, suffix-free HIR data,
  unchanged compatibility versions, tests, and explicit non-goals.

  Approved 2026-09-04. The exact set is `i8`, `i16`, `i32`, `i64`, `u8`,
  `u16`, `u32`, `u64`, `f32`, and `f64`. `int`, `uint`, and `float` add no
  alias suffixes; distinct `byte` has no suffix. A suffix fixes the literal's
  source type, after which existing widening, overload, conversion, constant,
  optimizer, and package rules apply.

- [x] **32.2 — Frontend.** Implement atomic numeric-suffix lexing, parser
  literal classification, exact semantic typing and representability, signed
  minima, deterministic malformed-suffix recovery, suffix-free HIR lowering,
  HIR verification, and focused valid/invalid unit coverage.

  Completed 2026-09-04. A shared decoder recognizes the ten canonical suffixes
  and preserves malformed candidates as one diagnosed token. Semantic analysis
  fixes suffixed source types while retaining unsuffixed contextual behavior;
  widening, overloads, switches, checked conversions, signed minima, and static
  constants use that exact type. HIR erases suffixes and verifies the canonical
  boundary. The dedicated six-case unit target and all 216 development and
  sanitizer CTests pass. Compatibility versions remain unchanged. Completing
  32.2 does not authorize 32.3.

- [x] **32.3 — Integration.** Complete static-constant and optimizer behavior,
  assignment/return/array/operator/overload/switch/conversion coverage,
  x86-64/wasm32 LLVM verification, whole/separate/source-free packages,
  affected-only invalidation, Shuttle determinism, editor support, and
  user-facing documentation.

  Completed 2026-09-04. One end-to-end fixture covers every suffix plus exact
  and widening assignments, returns, arrays, operators, overloads, switches,
  checked conversion, `wrap`, `sat`, static constants, and native output.
  Canonical typed constants fold through MIR and source-free packages; raw and
  O2 x86-64/wasm32 LLVM verify. Shuttle proves whole/separate/source-free
  equivalence, affected-only invalidation, failed-output preservation, and
  relocated serial/parallel determinism. The editor and user documentation
  expose the implemented syntax. Both compiler configurations pass all 224
  CTests. Compatibility versions and Shuttle production code remain unchanged.
  Completing 32.3 does not authorize 32.4.

- [x] **32.4 — Exit audit.** Complete every suffix at zero, extrema, adjacent,
  invalid category/case/width/tail, floating rounding, signed-zero, subnormal,
  and range boundaries; verify malformed models, unchanged unsuffixed behavior,
  failed-output preservation, relocated serial/parallel determinism, and all
  compiler, Rust/shared Shuttle, editor, formatting, documentation, and
  repository gates.

  Completed 2026-09-04. The ten-case numeric unit target covers all suffixes at
  zero and integer extrema/adjacent/out-of-range values; signed minima and
  unsigned rejection; binary32/binary64 ties-to-even neighbors, signed zero,
  minimum subnormals, finite extrema, underflow, and overflow; the complete
  malformed suffix families; unchanged unsuffixed contexts; and the 4,096-byte
  token boundary. HIR rejects retained suffixes, invalid cores/types, and
  out-of-range canonical values; MIR rejects invalid typed bits. Invalid
  overload recovery retains its original diagnostics. Both configurations pass
  all 224 CTests, including 30 toolchain and 26 native Shuttle cases. All 43
  ordinary Rust tests, Rust 1.85 checking, warning-denied Clippy, Rust/C++
  formatting, nine editor tests per compiler, all 99 local Markdown link checks,
  and repository hygiene gates pass. Compatibility versions remain unchanged.

  **Stage 32 is complete.** Stage 33 is tracked below.

### Stage 33: Numeric literal notation

- [x] **33.1 — Contract.** Approve
  [numeric literal notation](docs/proposals/stage_33_numeric_literal_notation.md)
  covering scientific notation, lowercase binary/octal/hexadecimal prefixes,
  strict digit separators, Stage 32 suffix interaction, exact values, atomic
  recovery, notation-free HIR, unchanged compatibility, tests, and non-goals.

  Approved 2026-09-04. Exponents accept `e` and `E`; hexadecimal digits accept
  both cases, while prefixes and suffixes remain lowercase. Underscores occur
  singly between digits. Base literals are integers, scientific forms are
  floating, leading zeroes remain decimal, and hexadecimal digit consumption
  makes `0x1f32` an integer rather than a suffixed float. This checkpoint
  changes documentation only.

- [x] **33.2 — Frontend.** Implement a shared bounded spelling decoder, atomic
  tokenization and recovery, syntax classification, exact radix/exponent value
  checks, existing contextual and suffixed typing, canonical notation-free HIR
  lowering, HIR verification, diagnostics, and focused unit/check tests.

  Completed 2026-09-04. The shared decoder owns separator, radix, exponent,
  suffix, and error classification. Exact scalar evaluation handles scientific
  notation without host floating parsing; HIR accepts only canonical base-ten
  integer or normalized coefficient/exponent text. The 14-case numeric unit
  target and two target-specific frontend checks pass in both 226-test compiler
  configurations. Downstream emission, package, editor, and user-documentation
  claims remain deferred to 33.3. Compatibility versions are unchanged.

- [x] **33.3 — Integration.** Complete static constants, MIR folding and
  verification, x86-64/wasm32 LLVM and native behavior, whole/separate/source-
  free packages, affected-only invalidation, Shuttle determinism, editor
  support, and user-facing numeric/syntax documentation.

  Completed 2026-09-04. Canonical notation values fold to exact MIR bits and
  pass LLVM verification before and after O2 on both targets. Native and
  four-package tests agree across whole-project, separate-package, and source-
  free compilation; relocated serial/parallel artifacts match, affected-only
  rebuilds reuse independent packages, and invalid edits preserve completed
  outputs. Both compiler configurations pass 232/232 tests, including 31
  public Shuttle toolchain and 28 native Shuttle cases. All 43 ordinary Rust
  tests, Rust 1.85 checking, warning-denied Clippy, Rust/C++ formatting, 10
  editor tests per compiler, and all 100 local Markdown link checks pass. User
  numeric/syntax documentation is updated; compatibility versions remain
  unchanged.

- [x] **33.4 — Exit audit.** Complete radix boundaries, floating exact-value
  boundaries, separators in every valid and invalid position, malformed token
  families, extreme exponent/resource cases, HIR/MIR mutation coverage,
  failed-output preservation, relocated determinism, and all compiler,
  Rust/shared Shuttle, editor, formatting, documentation, link, and repository
  gates.

  Completed 2026-09-04. The 17-case numeric target covers all integer suffixes
  across binary, octal, and hexadecimal extrema and failures; exact scientific
  signed zero, ties, subnormals, finite extrema, near and extreme range errors;
  strict separator positions; atomic source ordering; and prefix, separator,
  exponent, and suffix bytes at the 4,096-byte limit. Both compiler
  configurations pass 232/232 tests, including 31 public Shuttle toolchain and
  28 native cases. All 43 ordinary Rust tests, Rust 1.85 checking,
  warning-denied Clippy, Rust/C++ formatting, 10 editor tests per compiler, all
  100 local Markdown target checks, and repository hygiene gates pass.
  Compatibility versions remain unchanged.

  **Stage 33 is complete.** Stage 34 is tracked below.

### Stage 34: Typed errors

- [x] **34.1 — Contract.** Approve
  [typed errors](docs/proposals/stage_34_typed_errors.md) covering file-wide
  error declarations, the compiler-known `Error` root and `DivisionByZero`,
  throw expressions, typed public throws sets, private inference, automatic
  propagation, constructor failure, inheritance/interface rules, portable ABI,
  compatibility, diagnostics, verification, and non-goals.

  Approved 2026-09-04. Calls retain ordinary syntax; Cloth adds no `try`,
  `catch`, `recover`, or `finally`. Public functions and constructors explicitly
  declare errors after the return type or parameter list, while private
  lowercase callables may infer their minimal transitive set. `throw` is an
  expression with an internal bottom type, so `T? ?? throw E()` produces
  non-null `T`. The planned implementation targets artifact format 5, compiler
  ABI 5, and runtime ABI 4 with an explicit portable result/error channel.
  Active compatibility constants and production behavior remain unchanged in
  this documentation-only checkpoint. All 101 local Markdown target checks and
  repository whitespace gates pass.

- [x] **34.2 — Frontend and interfaces.** Implement `error`, `throw`, and
  `throws`; error declarations and inheritance; parser/AST; semantic error-set
  validation and deterministic private fixed-point inference; bottom/null flow;
  constructor, override, interface, imported-call, and `Main` contracts; HIR;
  verification; diagnostics; and focused frontend tests.

  Completed 2026-09-05. The compiler recognizes and verifies typed errors
  through HIR, including class-like error inheritance, the compiler-provided
  `Error` and `DivisionByZero` types, explicit public throws sets, deterministic
  private inference, constructor/field/call effects, bottom-aware flow, and
  narrowing override/interface contracts. Native typed-error compilation stops
  at a targeted gate; imported typed-error metadata remains coupled to the
  34.3 artifact transition. Artifact/compiler/runtime versions remain 4/4/3,
  and Shuttle fixtures, editor support, and user documentation are unchanged.
  The focused target passes all 15 cases; development and ASan/UBSan each pass
  all 233 CTests.

- [x] **34.3 — Lowering and integration.** Implement verified MIR error edges,
  the result/error calling convention, GC-safe propagation and failed-
  construction cleanup, compiler-known descriptors, terminal reporting,
  integer division/remainder migration, artifact/compiler/runtime compatibility
  transitions, x86-64/wasm32 LLVM and native behavior, source-free packages,
  Shuttle determinism and preservation, editor support, and user documentation.

  Completed 2026-09-05. MIR represents error completion and throwing-call
  success/error edges; ABI 5 returns nullable errors and writes non-void results
  through result pointers. LLVM propagation retains GC roots, failed
  construction never publishes an object, and `Main` reports uncaught errors.
  Runtime ABI 4 provides compiler-known error descriptors and reporting;
  integer division and remainder by zero now produce `DivisionByZero`.
  Artifact format 5 preserves error kinds, bases, throws sets, and physical
  signatures for source-free consumers. Native x86-64, verified x86-64/wasm32,
  deterministic Shuttle, editor, and user-documentation coverage pass. Both
  compiler configurations pass all 241 CTests; the focused target passes all
  15 cases, Shuttle passes 32 protocol/toolchain and 29 native cases, all 43
  ordinary Rust tests, the Rust 1.85.0 check, warning-denied Clippy, all 12
  relevant editor checks, local Markdown targets, and repository whitespace
  checks pass.

- [x] **34.4 — Exit audit.** Complete error declaration, throw-expression,
  effect-set, constructor, override/interface, malformed-state, runtime output,
  division-by-zero, cross-target, package-determinism, and every compiler,
  Rust/shared Shuttle, editor, formatting, documentation, link, and repository
  quality matrix.

  Completed 2026-09-05. The audit expanded the focused target to 18 cases and
  closed imports, constructor field-flow, forged HIR/MIR/ABI, runtime error
  identity/tracing/rejection, every supported throwing success shape, failed
  construction, x86-64/wasm32 LLVM before and after O2, exact native behavior,
  and typed-error-specific Shuttle invalidation and failed-output preservation.
  It found and fixed user error imports being mistaken for core-type conflicts
  and a constructor-analysis crash on `throw`. Both compiler configurations
  pass all 249 CTests, including 32 public Shuttle toolchain cases and 30 native
  cases. All 43 ordinary Rust tests, Rust 1.85 checking, warning-denied Clippy,
  Rust/C++ formatting, 12 editor checks per compiler, local Markdown targets,
  and repository whitespace gates pass. Compatibility remains 5/5/4.

  **Stage 34 is complete.** Further work requires a separately proposed and
  approved stage.

### Stage 35: Standard library foundation

- [x] **35.1 — Contract.** Approve the
  [standard-library foundation](docs/proposals/stage_35_standard_library_foundation.md),
  including compiler/library ownership, the reserved `cloth` import root,
  canonical `std` source layout, explicit imports, automatic Shuttle
  dependency injection, exact version/digest inputs, distribution,
  diagnostics, verification, compatibility, and non-goals.

  Approved 2026-09-05. The `std` submodule owns library source. The official
  package identity is `cloth`, `src/math/Math.co` maps to
  `cloth.math::Math`, and ordinary source roots, dependency aliases, and import
  aliases cannot claim `cloth`. Shuttle will inject the selected library as an
  implicit direct dependency without implicitly importing its types. `Error`,
  `DivisionByZero`, primitives, and core runtime/ABI behavior remain compiler-
  owned. Compatibility remains 5/5/4 with process protocol 2, receipt schema 1,
  and manifest schema 1. This checkpoint changes documentation only.

- [x] **35.2 — Compiler and library bootstrap.** Enforce reserved-root and
  case-collision diagnostics; normalize the standard-library manifest and move
  its source root from `src/cloth/` to `src/`; compile `Math` as a library
  package without an executable or self-dependency; resolve the existing
  `Gcd`/`Lcm` dynamic integer division effects without weakening Stage 34; and
  verify canonical identity, interface/object artifacts, LLVM, native
  consumers, and both targets.

  Completed 2026-09-05. The compiler reserves `cloth` case-insensitively across
  source-package roots and package/import aliases while requiring the exact
  distinguished dependency `cloth -> cloth`. The `std` package is now the
  executable-free `cloth` v0.1.0 package, and `src/math/Math.co` owns
  `cloth.math.Math`; `Gcd` and `Lcm` explicitly declare `DivisionByZero`.
  Interface artifacts compile on x86-64 and wasm32, native object artifacts link
  through a source-free consumer, and LLVM passes verification before and after
  O2 on both targets. Development and ASan/UBSan configurations each pass all
  255 CTests. Compatibility remains 5/5/4 with protocol/schema versions 2/1/1.
- [x] **35.3 — Shuttle integration.** Select the standard-library distribution
  paired with the chosen compiler; inject the exact package into every ordinary
  package; preserve deterministic progress, reuse, invalidation, atomic
  publication, linking, and failure behavior; and add user/tooling
  documentation.

  Completed 2026-09-05. `clothc` advertises `cloth` v0.1.0 and CMake generates
  adjacent schema-1 toolchain metadata selecting the exact library manifest.
  Shuttle validates the paired executable-free package, injects it directly
  into every ordinary package, rejects manifest aliases and replacement
  packages, and carries its artifact through deterministic compile, reuse,
  receipt, and link paths. User-facing imports remain explicit and require no
  manifest entry.
- [x] **35.4 — Exit audit.** Complete namespace and shadowing diagnostics,
  absent/duplicate/corrupt/incompatible library handling, bootstrap and
  consumer matrices, whole/separate/source-free equivalence, relocated serial/
  parallel determinism, x86-64 native and x86-64/wasm32 verification, and all
  compiler, runtime, Shuttle, standard-library, editor, documentation,
  formatting, link, and sanitizer gates.

  Completed 2026-09-05. Both 255-test compiler configurations, all 35 public
  Shuttle toolchain cases, 32 native cases, 49 ordinary Rust tests, both-target
  artifact checks, editor checks, documentation links, formatting, Clippy,
  Rust 1.85, and repository gates pass. The audit record is in
  [testing](docs/testing.md#stage-354-standard-library-foundation-exit-audit).

### Stage 36: Standard-library prelude

- [x] **36.1 — Prelude contract.** Define `cloth.lang` eligibility, exact
  reach, deterministic lookup precedence, compiler/Shuttle
  ownership, source-free behavior, compatibility, evolution, diagnostics,
  verification, and non-goals.

  Approved and completed 2026-09-05. The prelude is a low-priority type-name
  fallback derived from the canonical `cloth` declarations already supplied to
  the compiler. It adds no AST import, source discovery, dependency edge,
  public API, version change, or production behavior in this checkpoint. See
  the [contract](docs/proposals/stage_36_standard_library_prelude.md).
- [x] **36.2 — Prelude resolution.** Implement whole-project and imported-
  artifact lookup for public direct `cloth.lang` file types. Cover all file
  kinds, capitalization, precedence, explicit imports, wildcard ambiguity,
  core conflicts, absent inputs, malformed models, source-free packages, and
  deterministic diagnostics without a Shuttle production change.

  Completed 2026-09-05. Semantic analysis builds one canonical, public,
  direct-`cloth.lang` fallback table after registering source and verified
  artifact declarations. Normal bindings remain authoritative; invalid core
  collisions are reported once at the library declaration. Synthetic compiler
  and public Shuttle fixtures cover both targets without adding production
  standard-library source or changing compatibility versions.
- [x] **36.3 — Initial `lang` API slice.** Approve the exact public declarations
  separately, then add only that source-defined set beneath `std/src/lang/`,
  select its library version, document it for users, and verify artifacts,
  both targets, native execution, reuse, and invalidation.

  Completed 2026-09-05 and amended 2026-09-06. Prelude lookup now recursively
  covers public types beneath `cloth.lang` and rejects duplicate public short
  names across that tree. `cloth.lang.errors.ArgumentError` and
  `cloth.lang.errors.StateError` are ordinary extensible errors with default
  and message constructors. The paired package remains `cloth` v0.2.0; exact
  version and digest flow through existing capability, metadata, artifact,
  receipt, cache, and link contracts without schema changes.
- [x] **36.4 — Exit audit.** Complete lookup, bootstrap, compatibility,
  malformed-input, whole/separate/source-free, failure-preservation, relocated
  serial/parallel, x86-64 native, x86-64/wasm32, editor, documentation,
  formatting, link, sanitizer, and repository quality matrices.

  Completed 2026-09-06. Development and sanitizer configurations each pass all
  255 CTests, including 36 compiler-backed Shuttle cases and 32 native cases.
  All 49 ordinary Rust tests, Rust 1.85, warning-denied Clippy, both 12-test
  editor runs, both-target LLVM verification, local documentation links,
  formatting, and repository gates pass. The audit added scoped sanitizer
  headroom for the multi-artifact standard-library integration test; no
  production behavior or compatibility version changed.

  **Stage 36 is complete.**

### Stage 37: Portable program arguments

- [x] **37.1 — Contract.** Define eligible zero- and one-parameter `Main`
  signatures, argument value and encoding semantics, entry selection,
  runtime/GC ownership, Shuttle forwarding, compatibility, diagnostics,
  verification, and non-goals.

  Approved and completed 2026-09-06. An entry may take exactly one non-null
  `string[]` of non-null application arguments. Values exclude the executable
  name, preserve order and empty values, and use strict Unicode conversion.
  Shuttle owns an explicit `run --` forwarding boundary. Artifact/compiler
  versions and schemas remain unchanged; the required owned-value runtime
  operation advances runtime ABI 4 to 5 during 37.2. See the
  [contract](docs/proposals/stage_37_program_arguments.md).
- [x] **37.2 — Compiler and runtime.** Accept the argument-taking entry shape,
  construct and root owned managed argument values, update whole-project and
  package entry adapters, advance runtime ABI to 5, and verify malformed state.

  Completed 2026-09-06. Direct and source-free native adapters accept the exact
  entry signature, construct owned arguments through runtime ABI 5, preserve
  typed-error and status behavior, root the array across invocation, and reject
  malformed entry ABI or host text. Existing zero-parameter entries remain
  unchanged.
- [x] **37.3 — Shuttle integration.** Forward host-native values after
  `shuttle run --`, preserve streams and statuses, and prove direct/Shuttle and
  whole/separate/source-free equivalence.

  Completed 2026-09-06. Only `run` accepts application values, only after an
  explicit delimiter. Shuttle stores and forwards `OsString` values directly,
  leaves build inputs and cache keys unchanged, and preserves application
  streams and statuses. Exact values, zero arguments, reuse, malformed Unicode,
  direct execution, whole-project compilation, and source-free linking pass.
- [x] **37.4 — Exit audit.** Complete strict encoding, resource, GC,
  compatibility, failure-preservation, native/cross-target, Rust, editor,
  documentation, formatting, and repository quality matrices.

  Completed 2026-09-06. Both 269-test compiler configurations pass, including
  36 compiler-backed Shuttle cases and 33 native cases per configuration.
  Coverage closes all entry forms and invalid shapes, strict host text,
  resource failures, construction and in-`Main` collection, direct/whole/
  separate/source-free equivalence, exact forwarding, status/error behavior,
  deterministic reuse, and stale-output prevention. Rust 1.85, Clippy, editor,
  documentation, formatting, and repository gates pass. Compatibility remains
  artifact/compiler/runtime 5/5/5 and process/receipt/manifest/toolchain schemas
  2/1/1/1. **Stage 37 is complete.**

### Stage 38: Portable text input and primitive parsing

- [x] **38.1 — Contract.** Freeze `Console.ReadLine`, line and strict Unicode
  semantics, primitive `T::parse`, exact grammar and rounding, `IoError` and
  `ParseError`, the trusted standard-library bridge, memory ownership,
  compatibility, diagnostics, verification, and non-goals.

  Approved and completed 2026-09-06. `Console.ReadLine(): string? throws
  IoError` is an explicitly imported ordinary member of `cloth.io.Console`.
  Existing primitive targets gain lowercase `::parse(string)` operations that
  throw the source-defined prelude type `ParseError`. Parsing is strict,
  locale-independent, consumes the complete string, and reuses Stage 33 numeric
  notation without suffixes. The bridge is private to the exact compiler-paired
  `cloth` package and is not a general FFI. This checkpoint changes
  documentation only; active compatibility remains 5/5/5 and 2/1/1/1 with
  `cloth` v0.2.0. See the
  [contract](docs/proposals/stage_38_text_input_and_parsing.md).
- [x] **38.2 — Library and runtime foundation.** Add `Console`, `IoError`, and
  `ParseError`; implement the private library bridge and complete checked
  line-input and primitive-parsing runtime operations; advance runtime ABI to 6
  and `cloth` to v0.3.0; and verify the low-level boundary independently.

  Completed 2026-09-06. The compiler-paired `cloth` v0.3.0 package now defines
  `cloth.io.Console`, `IoError`, and `ParseError`. The exact private bridge
  lowers `Console.ReadLine` through runtime ABI 6 and maps recoverable statuses
  to source-defined `IoError` values. The same ABI provides the complete strict,
  locale-independent primitive parser substrate without exposing `T::parse`
  before 38.3. Development and sanitizer coverage verifies the boundary.
- [x] **38.3 — Primitive parsing integration.** Implement every approved
  `T::parse` operation through semantic analysis, verified HIR/MIR, checked
  lowering, whole/source-free packages, native and both-target paths, Shuttle
  input inheritance, editor support, and user documentation.

  Completed 2026-09-06. All approved canonical primitive types and the
  `int`/`uint`/`float` aliases expose strict lowercase `T::parse(string): T
  throws ParseError`. Typed effects, source-defined errors, checked runtime-ABI-6
  lowering, whole/source-free artifacts, both targets, native execution,
  Shuttle stdin inheritance and reuse, editor grammar, and user documentation
  are integrated without a compatibility or schema transition.
- [x] **38.4 — Exit audit.** Complete Unicode, line, grammar, rounding,
  resource, GC, compatibility, determinism, failure-preservation, native/
  cross-target, Rust, editor, documentation, formatting, link, sanitizer, and
  repository quality matrices.

  Completed 2026-09-06. The audit closes line-ending, EOF, chunking, Unicode,
  embedded-null, complete-input grammar, integer-boundary, floating-rounding,
  boolean/character, malformed-layout, typed-effect, once-evaluation, GC,
  native/cross-target, artifact, Shuttle, determinism, invalidation, reuse, and
  failure-preservation matrices. All coordinated development, sanitizer,
  Rust/MSRV, editor, documentation, formatting, link, and repository gates
  pass without a compatibility, schema, or library-version transition.

## Unscheduled backlog

These entries are intentionally unnumbered. They cannot be pulled into an
active stage without first updating `ROADMAP.md`.

### Language and object model

- Define immutable per-case constant data separately from per-value payloads.
  Struct-valued case data depends on approved struct and constant-initialization
  contracts; enum identity/equality must remain independent of attached data.
- Evaluate payload-bearing enums, enum members and conformance, explicit
  discriminants/underlying types, checked numeric conversion, and flags only
  under future contracts. These are not implied by Stage 25's named-value model.
- Design case enumeration and enum reflection separately from Stage 25's typed
  printing and `::typeName` contract; do not expose internal tags as a stable
  serialization format.
- Struct foundations, native lowering, and the Stage 26 exit audit are complete.
  Mutating struct receivers, struct conformance/boxing, reference returns, and
  user-defined copy/move hooks remain future contracts unless the approved
  Stage 26 scope explicitly changes.
- Implement nested type declarations. `class`, `struct`, and `enum` are
  reserved declaration starters and currently diagnosed as unsupported.
- Add flow-sensitive smart casts after a successful `value is T` test.
- Add primitive boxing before primitives may widen to `object`.
- Define the reflection surface beyond the stable `::typeName` meta query.
- Evaluate generics, traits, or an interface-based alternative after the Cloth
  1.0 boundary.
- Add first-class function values.
- Design user-defined conversions without weakening lossless implicit numeric
  conversion or checked built-in conversion.
- Decide whether Cloth synthesizes implicit default constructors.
- Decide whether distinct unit and never types belong beside `void`.
- Add reflection ability to allow "::name" meta access on declarations to return
  a `string` representation of the declaration name (if applicable).

### Expressions, numeric operations, and control flow

- Switch statements and enum exhaustiveness are complete under Stage 27 above.
  Pattern matching, destructuring, guards, ranges, and value-producing switch
  expressions remain deferred and require separate contracts.
- Design wrapping and saturating arithmetic separately from Stage 30's explicit
  conversion modes. No arithmetic spelling is reserved by the conversion
  contract.
- Add floating-point bit representation and byte-order operations after the
  integer-only Stage 21 boundary is proven.
- Align wider Unicode character literals/escapes and artifact constants across
  lexer, scalar decoding, and emission. Current literal/artifact handling is
  byte-oriented despite `char` having 32-bit storage; Stage 28 preserves that
  boundary rather than implicitly expanding the character language.
### Optimization

- Add ability to make full qualified name calls.

### Nullability

- Add nullable value types. This is required before safe access or safe meta
  queries can produce nullable primitive values.
- Add safe function calls on nullable receivers. Until then, callers narrow or
  assert the receiver first.

### Arrays, iteration, and collections

- Add contextual typing for empty and null-only array literals.
- Reify complete array element-type identity before checked `is`/`as` with
  array targets.
- Design multidimensional arrays, resizable lists, and slices without weakening
  fixed-length `T[]`.
- Add deep array equality as an explicit operation; `==` remains reference
  identity.
- Extend `for (... in ...)` beyond arrays only after defining the required
  range, binding, destructuring, async, or iterable contracts independently.
- Allow multidimensional array types (`object[][][]`).
- Allow setting default array size `object[:3]` (from 0 to 2, or 3 spots).

### Strings, formatting, and representation

- Add string indexing, slicing, and iteration over a deliberately selected
  Unicode unit.
- Add interpolation and type-checked formatting.
- Add case conversion, Unicode normalization, searching, and interning.
- Decide how a source-defined standard-library string API layers over the
  primitive immutable UTF-8 representation.
- Add array rendering and user-defined object-to-string formatting while
  preserving the stable default representation.

### Static storage and initialization

- Define deterministic static initialization order and collector root
  registration before dynamic initializers, mutable static fields, or
  reference-valued static fields.
- Define aggregate constant construction before struct-valued static constants
  or struct-backed enum metadata; Stage 26 construction does not imply compile-
  time execution or general constant folding.

### Packages, dependencies, and distribution

- Add Shuttle package registries, semantic-version selection, lockfile
  generation, and remote dependency retrieval after the local-only Stage 22
  contract.

### Backend, runtime, and tooling

- Add a WebAssembly runtime and linker path; wasm32 LLVM IR emission already
  exists.
- Expand native output beyond the current x86-64 pipeline.
- Add selectable optimization levels and debug information.
- Add platform packaging and distribution tooling.
- Add ability for Shuttle to build to .lib or .a (Linux/MacOS) for library files.

### Memory management

- Move compiler syntax and semantic/HIR storage to an arena or
  garbage-collected ownership model without making addresses identities.
- Reuse proven-dead shadow-stack root slots without changing the root-frame
  ABI.
- Add multi-mutator collection support before native threading.
- Evaluate finalizers, weak references, concurrent tracing, generational
  collection, and moving collection separately; each requires observable
  semantics and barrier design.
- Align the remaining VS Code new-file generators and legacy language claims
  with the current file-based language; the focused 26.5.1 override support does
  not redesign historical trait/library scaffolds.

## Intentional invariants and non-goals

These are current decisions, not unfinished work:

- Arrays are invariant; `User[]` does not widen to `object[]`.
- Cloth does not expose unchecked C-style variadic `printf`.
- Imports use identifier paths rather than string paths, and source files do
  not repeat package, module, or default class declarations.
- Object addresses, allocation identifiers, collector state, and reclamation
  timing are not source-visible behavior.
- `.` is declared member access; `::` is reserved for language-defined meta
  operations and qualified paths.
- Generics and traits are outside the Cloth 1.0 scope.
