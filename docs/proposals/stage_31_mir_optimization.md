# Proposal: Stage 31 MIR optimization

Status: **complete — 31.4 exit audit passed on 2026-09-04**.

This proposal defines Cloth's first target-independent optimizer. The user
approved the MIR optimization direction, constant folding, propagation, control-
flow cleanup, and staged delivery on 2026-09-03. The separately authorized
31.2 scalar-fold implementation and the separately authorized 31.3 CFG and
production-pipeline integration are complete. The separately authorized 31.4
exit audit closed Stage 31 on 2026-09-04.

See the [roadmap](../../ROADMAP.md#stage-31-mir-optimization) and
[work ledger](../../TODO.md#stage-31-mir-optimization).

## 1. Objective and pipeline boundary

Stage 31 adds deterministic scalar optimization without changing Cloth source
semantics. The production pipeline becomes:

```text
HIR -> MIR lowering -> MIR verification -> MIR optimization
    -> MIR verification -> ABI lowering -> ABI verification -> LLVM
```

The optimizer consumes only verified MIR and the immutable semantic model. It
runs before target layout and ABI lowering, so its decisions cannot depend on
the compiler host, x86-64, wasm32, object format, native linker, or LLVM
version. `CompilationResult::mir` contains the optimized module. Tests and
internal tools may call `lower_to_mir` to retain a baseline module for
comparison.

Optimization is an always-on correctness-preserving compiler phase. Stage 31
does not add a source feature, command-line switch, optimization level, build-
manifest field, environment variable, or production test bypass. `--check`
continues to stop after frontend analysis and does not construct MIR.

## 2. Observable-semantics rule

An optimized program must have the same:

- standard output, standard error, process status, and terminal failure text;
- left-to-right evaluation order and exactly-once side effects;
- allocation, call, store, checked access, and runtime guard behavior on every
  reachable execution path;
- fixed-width integer overflow, division, remainder, shift, checked-conversion,
  wrapping, and saturation semantics;
- IEEE binary32/binary64 rounding, signed zero, and subnormal behavior;
- branch, loop, switch, return, and short-circuit result; and
- source diagnostics and warnings, which are completed before optimization.

The optimizer never converts an ordinary runtime failure into a compile-time
error. If constant evaluation reports overflow, a zero divisor, invalid shift,
failed checked conversion, non-finite folding result, or another failure, the
original runtime MIR remains. Stage 31 does not replace these operations with a
generic trap because doing so could change the existing failure message or
guard ordering.

Unreachable blocks may be removed only after the reachable predecessor chooses
a constant successor. Effects in a reachable block are never discarded merely
because their results are unused. Calls, allocation, member/storage access,
array access, meta operations, type checks/casts, null assertions, field
initialization, integer byte-order operations, and mutable loads/stores are not
assumed pure.

## 3. Canonical folded constants

Stage 31.2 introduces a MIR scalar-constant instruction containing one verified
`ScalarConstant`: exact `TypeId` plus canonical bits. It represents `bool`,
`char`, fixed-width integers, finite floating-point values, and enum tags.
Strings, null, references, arrays, structs, and other aggregates are excluded.

The new instruction is distinct from a source literal. Existing MIR literals
retain their lexemes and source ranges; folded results do not invent source
text. The verifier requires:

- a result value whose type exactly equals the constant type;
- canonical bits for that semantic type;
- a valid owning enum and in-range tag for enum constants; and
- a scalar, non-nullable, non-aggregate value type.

LLVM lowering consumes the canonical bits directly. Floating-point constants
use exact integer-bitcast form, preserving signed zero and subnormals without a
host floating parser. MIR printing uses deterministic type and hexadecimal-bit
text. This internal instruction changes neither artifact metadata nor the
public language surface.

## 4. Scalar folding and propagation

Stage 31.2 computes a per-body scalar constant lattice with `unknown`, exact
typed constant, and `varying` states. A deterministic worklist may fold an
instruction only when all required operands have exact constants and the
existing scalar semantics return a valid result.

Eligible operations are:

- scalar source literals and loads of verified `static final` scalar values;
- unary `!`, `+`, `-`, and `~` where valid for the exact operand type;
- integer and finite floating arithmetic;
- integer bitwise operations and shifts;
- boolean operations and scalar comparisons/equality;
- numeric widening and checked numeric conversion; and
- integer `wrap` and `sat` conversion.

The pass reuses the Stage 28-30 scalar operations rather than implementing a
second arithmetic policy. Results retain the original MIR result ID, exact
type, and source range. A valid fold replaces only the pure result-producing
instruction with a canonical scalar constant. A failed or unsupported fold
leaves the instruction byte-for-byte unchanged and emits no user diagnostic.

Propagation may use imported scalar constants because source and source-free
artifacts expose the same verified `ScalarConstant`. It does not infer values
from mutable storage, final locals, object fields, array elements, function
bodies, or ABI layout.

## 5. Phi and control-flow simplification

Stage 31.3 extends the same analysis through control flow:

- a scalar phi becomes constant only when every reachable incoming value has
  the same exact type and canonical bits;
- a constant boolean branch becomes a jump to the selected successor;
- a constant integer or enum switch becomes a jump to its exact case or
  verified default successor; and
- blocks unreachable from the entry after those rewrites are removed.

Control-flow cleanup keeps surviving blocks and instructions in their original
relative order. It rebuilds block IDs, value IDs, terminators, storage paths,
phi predecessors, callable bodies, reachability flags, and value counts as one
canonical compaction. Removed predecessors are removed from phis; a single-
incoming phi aliases its incoming value without evaluating anything again.
Enum invalid-tag behavior remains reachable whenever the selector is not a
verified in-range enum constant.

Folding and control-flow cleanup iterate to a fixed point using bounded,
iterative worklists. The complete optimizer is idempotent: optimizing an
already optimized verified module produces structurally identical MIR.
Traversal order is file, callable, block, and instruction order; hash-table or
thread scheduling order cannot affect output.

## 6. Failure and resource behavior

The production pipeline verifies MIR before invoking the optimizer and verifies
the result again before ABI lowering. Invalid input is an internal compiler
error and never reaches optimization. Broken optimizer output is diagnosed as
invalid MIR; LLVM and native tooling are not invoked.

Analysis storage is linear in each body's block, instruction, value, phi-edge,
and control-flow-edge counts. Worklists are iterative rather than recursive.
Each value and edge has a bounded number of lattice transitions, and cleanup
uses indexed remapping rather than repeated full-body scans. Stress tests use
wide expressions, long SSA chains, diamonds, loops, and large switches to
guard this complexity without a wall-clock-only contract.

## 7. Packages and compatibility

Whole-project, selected-package, separate-package, and source-free compilation
all use the same optimizer. Package selection does not permit cross-package
body inspection. Only verified public scalar constants may propagate across an
artifact boundary.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and Shuttle manifest schema **1** remain unchanged.
Optimized object bytes are ordinary compiler output. The changed compiler
executable digest naturally invalidates artifacts produced by another compiler
binary; no additional cache key, capability, receipt field, or manifest option
is introduced. Existing artifacts remain structurally valid under their normal
exact-compiler compatibility checks.

Shuttle treats optimized interfaces, constants, and object code as opaque. It
must verify affected-only rebuilds, output preservation, and deterministic
serial/parallel artifacts without parsing MIR or optimizer metadata.

## 8. Verification requirements

Required coverage includes:

- every eligible scalar type and operator at zero, extrema, adjacent values,
  signed/unsigned crossings, floating signed zero/subnormals, and enum tags;
- successful folding plus unchanged runtime MIR for overflow, zero divisors,
  invalid shifts, failed checked conversions, and non-finite floating results;
- exact baseline/optimized output, error text, status, and side-effect counts;
- constants in static fields, imported/source-free values, expressions, phis,
  branches, switches, loops, returns, and call arguments;
- malformed canonical constants and corrupted post-pass MIR rejection;
- fixed-point idempotence, stable compaction, input-order independence, and
  x86-64/wasm32 LLVM verification before and after O2;
- whole-project, separate-package, and source-free equivalence, affected-only
  invalidation, failure preservation, and relocated serial/parallel artifact
  determinism; and
- development/sanitizer, Rust/shared Shuttle, editor, formatting,
  documentation, and repository quality gates.

Tests compare baseline and optimized internal pipelines without exposing an
unoptimized production mode. No alternate evaluator, runtime helper, external
dependency, or test-only compiler switch is introduced.

## 9. Checkpoints

1. **31.1 — Contract (complete):** freeze the optimizer boundary, observable
   semantics, canonical constants, eligible folds, CFG rules, compatibility,
   verification, and non-goals.
2. **31.2 — Scalar folds (complete):** add canonical MIR constants,
   deterministic scalar propagation/folding, verifier/backend support, and
   focused tests.
3. **31.3 — CFG and integration (complete):** add phi propagation,
   branch/switch cleanup, canonical compaction, default-pipeline and package
   integration.
4. **31.4 — Exit audit (complete):** close equivalence, failure, idempotence,
   stress, determinism, documentation, and every compiler/toolchain quality
   gate.

The separately authorized exit audit completed on 2026-09-04. Stage 31 is
closed; later work requires a separately approved stage.

## Non-goals

Stage 31 does not add numeric suffixes, aggregate constants, user-defined
compile-time functions, macros, inlining, interprocedural propagation, common-
subexpression elimination, loop optimization, vectorization, general
reachable-block dead-code elimination, LLVM pass controls, public optimization
levels, debug information, new targets, exceptions, enum metadata, or remote
dependencies. It does not promise faster compilation; optimizer performance is
bounded and measured separately from Shuttle's existing responsiveness
contract.
