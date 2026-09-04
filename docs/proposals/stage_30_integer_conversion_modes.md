# Proposal: Stage 30 integer conversion modes

Status: **complete — 30.4 exit audit passed on 2026-09-03**.

This proposal defines explicit wrapping and saturating integer conversions. The
user approved the `wrap` / `sat` vocabulary, target-owned meta syntax, and
integer-only scope on 2026-09-03. The separately authorized 30.2 checkpoint
implements the frontend and required constant behavior. The separately
authorized 30.3 checkpoint implements verified runtime lowering and package
integration. The separately authorized 30.4 exit audit closes the complete
matrix, documentation, determinism, and quality gates.

See the [roadmap](../../ROADMAP.md#stage-30-integer-conversion-modes) and
[work ledger](../../TODO.md#stage-30-integer-conversion-modes).

## 1. Source contract

The two forms are:

```cloth
Target::wrap(value)
Target::sat(value)
```

`Target` must name `int8`, `int16`, `int32`, `int64`, `byte`, `uint8`,
`uint16`, `uint32`, or `uint64`; the aliases `int` and `uint` are accepted as
`int32` and `uint32`. The argument must have a non-nullable integer type. The
result has exactly the named target type and is a value, never a writable
location.

`wrap` and `sat` are contextual meta-operation names, not keywords. They remain
valid declaration names outside this position. These operations are
compiler-provided, cannot be imported, shadowed, overridden, or called through
`.` syntax, and do not participate in overload resolution.

The argument is analyzed once without a target-type expectation. Consequently,
ordinary integer literal defaults remain intact: `int8::wrap(300)` converts an
`int32` value rather than first trying to form an invalid `int8` literal.

## 2. Conversion rules

Let `v` be the mathematical value of the source integer and `N` the target bit
width.

`Target::wrap(v)` computes the least nonnegative residue of `v` modulo `2^N`.
An unsigned target returns that value. A signed target interprets the resulting
N-bit pattern as two's-complement. Source signedness affects the mathematical
input, not the number of bits copied from the source.

`Target::sat(v)` clamps `v` to the inclusive mathematical range of `Target`.
Values inside the range are unchanged. Values below the minimum become the
minimum; values above the maximum become the maximum.

Examples:

| Expression | Result |
| --- | --- |
| `int8::wrap(300)` | `44` |
| `int8::sat(300)` | `127` |
| `uint8::wrap(-1)` | `255` |
| `uint8::sat(-1)` | `0` |
| `int8::wrap(uint8(255))` | `-1` |
| `int8::sat(uint8(255))` | `127` |

An in-range or same-type conversion is valid and preserves the value. Neither
operation fails because of range. Evaluation of the argument and all of its
side effects occurs exactly once.

The existing `Target(value)` form remains the checked numeric conversion. No
implicit conversion, arithmetic operator, or overload rule changes.

## 3. Constants and lowering

Required constant contexts evaluate `wrap` and `sat` using the same mathematical
rules as runtime code. A valid conversion stores only its canonical target bits
in a scalar constant artifact; it does not retain the source width or conversion
mode. Runtime-only expressions remain runtime operations even when composed
entirely from literals.

HIR and MIR must retain the explicit conversion mode and exact source and target
types. Their verifiers reject non-integer operands, invalid modes, incompatible
result metadata, and attempts to treat the result as a location.

LLVM lowering is target-independent:

- `wrap` uses the source's signed or unsigned mathematical interpretation,
  followed by the required extension, truncation, or same-width bit
  reinterpretation;
- `sat` compares in a domain that represents the source and both target bounds,
  then selects the appropriate bound or converted value; and
- neither form calls the checked-conversion failure helper or introduces a new
  runtime symbol.

Lowering must not introduce LLVM poison or depend on host integer width.
x86-64 and wasm32 IR must verify before and after optimization.

## 4. Diagnostics and compatibility

Malformed arity, a non-integer target, a non-integer or nullable argument, and
an unknown primitive meta operation are compile-time errors. Diagnostics name
the target and operation and report the found source type when one exists.
Range alone is never an error for `wrap` or `sat`.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and Shuttle manifest schema **1** remain unchanged.
Existing artifacts remain valid. Shuttle treats the resulting object code and
scalar constants as opaque compiler-owned data; it does not parse conversion
modes or add public protocol fields.

## 5. Verification requirements

Required coverage includes:

- every source/target pair across all signed and unsigned widths, including
  `byte`, `int`, and `uint` aliases;
- minimum, maximum, zero, adjacent, in-range, below-range, and above-range
  values, with signed-to-unsigned and unsigned-to-signed cases;
- exact compile-time/runtime agreement and ordinary checked-conversion
  regression coverage;
- exactly-once argument evaluation and use in locals, fields, arguments,
  returns, and `static final` scalar initializers;
- malformed AST/HIR/MIR rejection and verified x86-64/wasm32 LLVM before and
  after optimization;
- whole-project, separate-package, and source-free constant/function behavior,
  affected-only invalidation, output preservation, and relocated
  serial/parallel determinism; and
- development/sanitizer, Rust/shared Shuttle, editor, formatting, and repository
  quality gates.

Tests extend existing targets and helpers. No production test switch, new
dependency, runtime helper, or alternate implementation is introduced solely
for testing.

## 6. Checkpoints

1. **30.1 — Contract (complete):** freeze syntax, integer semantics, constants,
   lowering, compatibility, verification, and non-goals.
2. **30.2 — Frontend and constants (complete):** implement parsing,
   AST/sema/HIR, exact diagnostics, required constant evaluation, and verifier
   coverage.
3. **30.3 — Lowering and integration (complete):** implement verified MIR/LLVM,
   native and package/source-free behavior, and coordinated Shuttle coverage.
4. **30.4 — Exit audit (complete):** close the complete conversion matrix,
   determinism/documentation ledgers, and every quality gate.

Stage 30 is complete. Later work requires a separately approved contract and
does not inherit authorization from this stage.

## Non-goals

Stage 30 does not add wrapping or saturating arithmetic, floating-point
conversion modes, numeric suffixes, user-defined conversions, nullable integer
values, general constant folding, optimizer controls, exceptions, new targets,
or new public Shuttle behavior. Possible future arithmetic names such as
`wrapAdd` and `satAdd` are not reserved or specified here.
