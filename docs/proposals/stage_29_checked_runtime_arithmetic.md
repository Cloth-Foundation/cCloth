# Proposal: Stage 29 checked runtime integer arithmetic

Status: **complete — 29.1 through 29.4 passed as of 2026-09-03**.

This proposal defines the runtime counterpart to Stage 28's checked constant
integer arithmetic. The user approved its source, failure, lowering,
compatibility, and non-goal contracts on 2026-09-03. The checked backend/runtime
transition and later checkpoints were implemented after their separate go-aheads.

See the [roadmap](../../ROADMAP.md#stage-29-checked-runtime-integer-arithmetic)
and [work ledger](../../TODO.md#stage-29-checked-runtime-integer-arithmetic).

## Objective and current boundary

Make ordinary integer arithmetic secure and deterministic: an operation either
produces its mathematical fixed-width result or terminates through Cloth's
runtime-failure path before LLVM can execute an invalid operation. The rule must
be identical for direct expressions, prefix/postfix updates, compound assignments,
array/member targets, whole-project compilation, and artifact-only dependencies.

Before 29.2, runtime `+`, `-`, `*`, and unary `-` lowered to plain fixed-width
LLVM operations, while division and remainder lowered directly to `sdiv`,
`udiv`, `srem`, or `urem`. The 29.2 implementation replaces that behavior for
direct expressions. Stage 28's required compile-time constants retain their
separate diagnostic policy.

## 1. Source semantics

The checked runtime policy applies to `byte`, `int8`, `int16`, `int32`, `int64`,
`uint8`, `uint16`, `uint32`, and `uint64`. `int` and `uint` remain aliases of
`int32` and `uint32`. It covers:

| Source form | Runtime rule |
| --- | --- |
| `left + right`, `left - right`, `left * right` | Trap unless the mathematical result fits the resolved result type. |
| `-value` | Trap for a signed minimum or a nonzero unsigned value. Zero remains valid for every integer type. |
| `left / right` | Trap on zero; also trap for signed minimum divided by `-1`. Otherwise truncate toward zero. |
| `left % right` | Trap on zero; also trap for signed minimum remaindered by `-1`. Otherwise use the dividend's sign. |
| `++value`, `value++`, `--value`, `value--` | Apply the same checked addition/subtraction rule with one. |
| `+=`, `-=`, `*=`, `/=`, `%=` | Apply the corresponding checked binary rule before storing. |

The resolved operand/result type remains the one selected by existing contextual
literal, lossless widening, and compound-assignment rules. Checks occur at that
type's exact width; calculation is not promoted to a host `int` or deferred until
a later conversion.

An ordinary runtime expression made entirely from literals is not a required
compile-time constant. It remains valid source and traps only if execution reaches
it. Stage 29 does not introduce general folding, `constexpr` inference, or new
diagnostics based on whether an optimizer happens to evaluate an expression.
`static final` scalar initializers retain Stage 28's compile-time diagnostics and
never defer a known invalid value to startup.

A leading minus that forms a signed integer literal is part of that literal's
value, so every signed minimum remains constructible. Negating an already formed
runtime value—including a variable holding that minimum—is checked arithmetic.

Floating-point arithmetic retains IEEE behavior, including infinities, NaNs,
signed zero, and division by zero. String concatenation, characters, booleans,
enums, comparisons, and numeric conversions retain their current contracts.

### Bit operations

Stage 21 remains authoritative for `&`, `|`, `^`, `~`, `<<`, and `>>`. Left
shift discards high bits by design and is not checked multiplication. Dynamic
shift counts continue to trap unless they are in `[0, left_width)`. Signed right
shift remains arithmetic and unsigned right shift remains logical. The compound
bitwise/shift forms preserve those rules.

Wrapping and saturating arithmetic or conversion remain explicit future primitive
meta operations. Stage 29 does not provide a switch that weakens default checks.

## 2. Evaluation and failure behavior

Preserve existing source evaluation order:

1. Evaluate the left operand or update target location once.
2. Evaluate the right operand once.
3. Validate the operation at the resolved integer type.
4. Produce/store the result only when validation succeeds.

An update or compound target—including an array element or projected field—is
resolved exactly once. A failed operation performs no target store. Prefix and
postfix forms retain their existing successful result: prefix yields the new value;
postfix yields the previous value. Earlier operand side effects remain observable,
but no later store or expression effect occurs after the runtime terminates.

Failures use the existing unrecoverable runtime path: write exactly one concise
line to standard error, then abort with a nonzero process status. Stage 29 freezes
these messages:

- `cloth runtime error: integer arithmetic overflow`
- `cloth runtime error: integer division by zero`
- `cloth runtime error: integer remainder by zero`

The signed-minimum/`-1` division and remainder cases report integer arithmetic
overflow. No source exception, `try`/`catch`, unwinding, recovery, or numeric
process-status value is introduced. Recoverable exceptions remain a separate
language/runtime/ABI design.

## 3. MIR and LLVM lowering

MIR continues to carry the typed source operation. Checkedness is the universal
meaning of integer arithmetic, not an optional flag that malformed MIR can disable.
The MIR verifier must reject nonnumeric operands, mismatched result types, invalid
operators, and inconsistent update/compound lowering before code generation.

For each supported width, LLVM emission uses the signed or unsigned overflow
intrinsic matching the semantic type:

- `llvm.sadd.with.overflow`, `llvm.ssub.with.overflow`, and
  `llvm.smul.with.overflow` for signed integers;
- `llvm.uadd.with.overflow`, `llvm.usub.with.overflow`, and
  `llvm.umul.with.overflow` for unsigned integers and `byte`; and
- signed/unsigned subtraction from zero for unary negation.

The intrinsic returns the fixed-width result and an overflow bit. The compiler
passes the inverse validity predicate to a runtime guard before the result can be
stored or otherwise observed.

Division and remainder must validate a nonzero divisor before emitting the LLVM
operation. Signed operations also validate that the pair is not minimum/`-1`.
Only after both guards return may the backend emit `sdiv`, `udiv`, `srem`, or
`urem`. No invalid operation may exist on an executable LLVM path before its
guard. The emitted predicates use the operand width and signedness directly;
neither C++ arithmetic nor a wider host type is a semantic oracle.

Add one C runtime boundary:

```c++
void cloth_rt_require_integer_arithmetic(std::uint8_t valid,
                                         std::uint8_t reason) noexcept;
```

The compiler-owned reason codes are `0` for overflow, `1` for division by zero,
and `2` for remainder by zero. A false predicate selects the exact message above.
An unknown failure reason is itself a runtime-contract violation. A true predicate
returns without side effects. The declaration is intentionally not `readnone` or
speculatable, so optimization cannot move a store or observable operation across
the guard.

LLVM verification must cover x86-64 and wasm32. Native execution is initially
x86-64 because the wasm32 runtime/linker remains deferred. Optimized-IR tests may
run LLVM's optimizer as verification, but Stage 29 does not expose an optimization
level or add the Cloth optimizer.

## 4. Compatibility and package boundary

The new runtime symbol changes the required compiler/runtime interface. Stage 29
therefore uses runtime ABI **3**. Artifact format **4**, compiler ABI **4**,
process protocol **2**, receipt schema **1**, and Shuttle manifest schema **1**
remain unchanged.

Compiler-owned package compatibility records, readers, writers, and artifact
goldens transition to runtime ABI 3 together. Existing format-4/runtime-ABI-2
packages require rebuilding; readers reject them with the ordinary
unsupported-runtime-ABI diagnostic rather than guessing compatibility. The exact
compiler and native-runtime digests continue to protect object reuse.

Runtime ABI is private compiler artifact metadata; public compiler capabilities
and receipts do not expose it. Shuttle therefore keeps artifacts opaque and
delegates runtime-ABI validation to `clothc` during inspect, reuse, and link. It
does not inspect MIR, arithmetic operations, guard reason codes, or object
payloads. Capability, receipt, stub, process, and manifest schemas do not change.

## 5. Verification requirements

Required coverage includes:

- minimum, maximum, zero, and adjacent values for every signed/unsigned width
  and `byte`, across addition, subtraction, multiplication, negation, division,
  and remainder;
- both division/remainder failure classes, including signed minimum with `-1`,
  with proof that invalid LLVM operations are not reached;
- direct expressions, return values, locals, fields, array elements, prefix and
  postfix updates, and every arithmetic compound assignment;
- target/operand evaluation exactly once, successful prefix/postfix values, no
  store on failure, left-to-right effects, and no guard on float/string/bitwise
  operations;
- compile-time Stage 28 diagnostics versus runtime failure for equivalent
  executed operations, including literal-only ordinary expressions;
- exact runtime messages, nonzero termination, clean stdout, and no execution of
  statements following a failed operation;
- well-formed guarded LLVM on x86-64/wasm32 before and after LLVM optimization,
  plus malformed HIR/MIR rejection and preexisting conversion/shift traps;
- whole-project, separate-package, and source-free dependency behavior, stale
  runtime-ABI rejection, affected-consumer invalidation, unrelated reuse, failed
  output preservation, and relocated serial/parallel determinism; and
- all compiler configurations, runtime/unit/integration regressions, Rust and
  shared Shuttle suites, editor checks, formatting, and whitespace checks.

Tests extend existing executables, fixtures, and launch helpers. No production
test switch, arbitrary timeout increase, new dependency, or alternate arithmetic
implementation is admitted solely for testing.

## 6. Stages and exit gates

1. **29.1 — Contract (complete):** approve this source, failure, lowering, compatibility,
   test, and non-goal contract; synchronize compiler and Shuttle ledgers. No
   production behavior changes in this checkpoint.
2. **29.2 — Checked lowering (complete):** implement the overflow/divisor guards, runtime
   boundary, runtime-ABI-3 transition, verifier hardening, and direct expression
   tests as one coordinated compiler/runtime/Shuttle change.
3. **29.3 — Updates and integration (complete):** verify prefix/postfix and every
   compound target with exactly-once behavior; complete native, package,
   source-free, compatibility, optimized-IR, and failure-output coverage.
4. **29.4 — Exit audit (complete):** close the boundary matrix, malformed-input and
   deterministic-build tests, docs and ledgers; pass both compiler configurations,
   Rust/shared-toolchain/native, editor, format, and repository checks.

All four checkpoints passed on 2026-09-03. No later stage is assigned or active.

## Non-goals and follow-on prerequisites

No floating runtime-policy change, wrapping/saturating operation, numeric suffix,
general constant folding, compile-time user function, optimization level, debug
location/runtime stack trace, recoverable exception, concurrency change, new
target/runtime, aggregate arithmetic, user-defined operator, or unrelated
Shuttle/editor feature is part of Stage 29.

Checked runtime semantics are a prerequisite for a future optimizer: folding may
replace an operation only while preserving its value, evaluation order, and trap.
Wrapping/saturating meta operations can be designed later against this checked
default. Neither follow-on is scheduled by this proposal.

Division by zero is also recorded for the future exception model. The intended
source direction treats an error as a file-wide nominal kind, constructed like a
class and implicitly derived from a universal `Error` base, for example:

```cloth
// DivisionByZero.co
error {
  DivisionByZero() {}
}
```

The approved [Stage 34 typed-error contract](stage_34_typed_errors.md) supersedes
that provisional direction. It selects a compiler-provided `Error` root,
`DivisionByZero`, typed automatic propagation through an explicit portable
result/error ABI, and no catching or platform unwinding. Stage 29 itself neither
reserves the keywords nor introduces those semantics. Until the separately
authorized Stage 34 lowering checkpoint is implemented, division by zero uses
the terminal runtime failure specified above; it must never reach an invalid
LLVM division.
