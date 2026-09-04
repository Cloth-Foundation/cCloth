# Proposal: Stage 32 typed numeric literals

Status: **complete — 32.4 exit audit passed 2026-09-04**.

This proposal defines optional, width-explicit suffixes for decimal numeric
literals. The user approved the typed-literal direction and activated the 32.1
contract checkpoint on 2026-09-04. The separately authorized 32.2 frontend,
32.3 integration, and 32.4 exit audit are complete.

See the [roadmap](../../ROADMAP.md#stage-32-typed-numeric-literals) and
[work ledger](../../TODO.md#stage-32-typed-numeric-literals).

## 1. Source syntax

The canonical suffixes are:

| Suffix | Exact type |
| --- | --- |
| `i8` | `int8` |
| `i16` | `int16` |
| `i32` | `int32` |
| `i64` | `int64` |
| `u8` | `uint8` |
| `u16` | `uint16` |
| `u32` | `uint32` |
| `u64` | `uint64` |
| `f32` | `float32` |
| `f64` | `float64` |

A suffix is lowercase, immediately adjacent to the literal, and part of the
numeric token:

```cloth
var small = 10i8;
var count = 10u64;
var ratio = 1.5f32;
var wholeRatio = 1f64;
```

An integer suffix may follow only an existing decimal integer spelling. A
floating suffix may follow either a decimal integer core such as `1f32` or an
existing decimal floating core such as `1.0f32`. An integer core with `f32` or
`f64` is a floating literal directly; it is not an integer-to-floating
conversion. A floating core cannot use an integer suffix.

The aliases `int`, `uint`, and `float` do not add `i`, `u`, or `f` suffixes;
their canonical widths are already represented by `i32`, `u32`, and `f32`.
`byte` remains a distinct unsigned eight-bit type and deliberately has no
suffix: `1u8` has type `uint8`, not `byte`. A `byte` literal continues to use
contextual typing or `byte(value)`.

Suffix sequences are contextual parts of numeric tokens, not keywords or type
names. Identifiers such as `i8` remain subject to the ordinary identifier rules
when they are not attached to a number.

## 2. Exact typing and representability

A suffix fixes the literal's initial semantic type. Expected-type propagation
must not retarget it. After the literal is formed, ordinary lossless widening,
common-type, assignment, return, array, switch-label, and overload rules apply
to the resulting typed value.

```cloth
var exact = 1i64;       // int64
int64 widened = 1i8;    // int8 widens to int64.
int8 rejected = 1i32;   // Invalid implicit narrowing.
```

Overload resolution treats a suffixed literal as an ordinary exact typed
argument, not as an adaptable unsuffixed literal. An exact parameter wins under
the existing rules; lossless widening may make another candidate compatible;
narrowing and signedness changes do not.

The numeric core must be representable in the selected type. Integer limits use
the existing fixed-width ranges. The immediately applied unary minus remains a
separate token but participates in the existing signed-minimum rule, so
`-128i8`, `-32768i16`, `-2147483648i32`, and
`-9223372036854775808i64` are valid. Their positive magnitudes are otherwise
out of range. Unsigned suffixes do not admit a negative value. Parentheses and
unary signs retain the existing numeric-literal-expression grouping rules and
cannot change the suffix-selected type.

Floating suffixes round the decimal value once to the selected IEEE-754 format,
using round-to-nearest with ties to even. Results must be finite. Signed zero,
subnormals, and the existing nonzero-literal-underflow rejection remain
unchanged. `-0.0f32` therefore produces exact negative binary32 zero.

Unsuffixed behavior is unchanged: an unconstrained integer defaults to `int32`,
an unconstrained decimal floating literal defaults to `float64`, and an
expected numeric type may select another representable type. A suffix does not
change any primitive width, arithmetic result rule, overflow behavior, checked
conversion, `wrap`, or `sat` semantics.

Explicit conversion operates on the suffix-selected source type. For example,
`int64(1i8)` converts an `int8` value, while `int8(128i16)` fails the existing
checked constant conversion. `Target::wrap(value)` and `Target::sat(value)` also
receive the exact suffixed argument type.

## 3. Lexing, recovery, and diagnostics

The lexer consumes a valid suffix as part of one integer or floating literal
token. The token range and source spelling include the suffix. The complete
token, including its suffix, remains subject to the existing 4,096-byte limit
in required constant contexts.

An identifier-continuation sequence directly after a numeric core is consumed
as a candidate suffix. It must match one complete canonical suffix and end at a
non-identifier character. This prevents accidental token splitting:

```cloth
1i32value  // Invalid suffix, not `1i32` followed by `value`.
1I32       // Invalid: suffixes are case-sensitive.
1f16       // Invalid: unsupported width.
1.0i32     // Invalid: integer suffix on a floating core.
```

Whitespace or a comment ends the numeric token, so `1 i32` is not a suffixed
literal and is diagnosed by the ordinary expression grammar. Existing member,
meta, delimiter, and operator punctuation remains a token boundary.

Diagnostics distinguish:

- an unknown or noncanonical suffix, naming its complete spelling;
- an integer suffix applied to a floating core;
- a value outside the suffix-selected type, naming the literal and type; and
- an otherwise valid typed value rejected by an existing assignment,
  conversion, operator, switch, or overload rule.

Recovery consumes the malformed candidate suffix once and resumes at its
following token boundary. Diagnostics are source ordered and deterministic;
they do not reinterpret an invalid suffix as identifiers or declarations.

## 4. Compiler representation

The syntax tree retains the complete source spelling and range. Semantic
analysis resolves a suffixed literal to the exact existing `TypeId`. At the HIR
boundary, numeric literal data uses the suffix-free decimal core plus that
resolved type; MIR, scalar evaluation, optimization, ABI lowering, and LLVM
therefore consume the existing typed-literal representation rather than parsing
source syntax again.

Static constants and folded MIR constants retain only their existing exact type
and canonical bits. Imported packages do not preserve or reconstruct the
suffix. Source-owned, whole-project, separate-package, and source-free
compilations must consequently agree without a new artifact field.

The lexer and semantic analyzer reject malformed source combinations rather
than allowing a suffix to reach HIR. HIR and MIR verification must require a
suffix-free numeric core that is valid for the expression's resolved type, so a
malformed internal model cannot defer source decoding to LLVM emission.

## 5. Compatibility and tooling

Typed suffixes add no keyword, runtime symbol, target-specific behavior,
command-line option, compiler capability, Shuttle manifest setting, receipt
field, or cache-key component. Adjacent number/identifier text was not valid
Cloth syntax before this stage, so consuming it as one diagnosed numeric token
does not change a valid program.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and Shuttle manifest schema **1** remain unchanged.
The compiler executable digest provides normal invalidation across compiler
versions. Shuttle treats the source syntax and resulting typed values as opaque
compiler concerns.

The VS Code grammar highlights the complete literal atomically. User-facing
integer, floating-point, syntax, casting, and operator documentation is updated
only when the corresponding implementation checkpoint makes the syntax usable.

## 6. Verification requirements

Required coverage includes:

- all ten suffixes at zero, minimum, maximum, adjacent, and out-of-range values;
- signed minima, unsigned rejection, binary32/binary64 rounding, signed zero,
  subnormals, finite limits, and underflow/overflow diagnostics;
- `1f32` and decimal-core floating forms, plus invalid category, case, width,
  repeated, and identifier-tail suffixes;
- unchanged unsuffixed defaults and contextual typing;
- exact and widening assignment, return, array, operator, overload, switch,
  checked-conversion, `wrap`, and `sat` behavior;
- static constants, canonical MIR folding, suffix erasure at HIR, malformed
  model rejection, and x86-64/wasm32 LLVM verification before and after O2;
- whole-project, separate-package, and source-free equivalence, affected-only
  invalidation, failed-output preservation, and relocated serial/parallel
  determinism; and
- development/sanitizer, Rust/shared Shuttle, editor, formatting,
  documentation, and repository quality gates.

Tests extend existing targets and shared launchers. No public unsuffixed mode,
alternate numeric evaluator, runtime helper, or external dependency is added
for testing.

## 7. Checkpoints

1. **32.1 — Contract (complete):** freeze syntax, exact typing,
   representability, token recovery, representation, compatibility, tests, and
   non-goals.
2. **32.2 — Frontend (complete):** implement lexing, parsing, semantic typing,
   HIR canonicalization, diagnostics, and focused model tests.
3. **32.3 — Integration (complete):** complete constants,
   MIR/optimizer/LLVM, package and Shuttle behavior, editor support, and user
   documentation.
4. **32.4 — Exit audit (complete):** close the full boundary and diagnostic
   matrices, cross-target/package determinism, and every quality gate.

Stage 32 is complete. Any extension to literal syntax or semantics requires a
future scheduled contract.

## Non-goals

Stage 32 does not add hexadecimal, binary, or octal literals; exponent notation;
digit separators; uppercase or alias suffixes; a `byte` suffix; arbitrary-
precision, decimal, half, or complex types; user-defined units or suffixes;
implicit narrowing; new conversions or arithmetic; aggregate constants;
constant functions; optimizer controls; reflection of source spelling;
exceptions; or unrelated tooling. Unary signs remain operators rather than
part of a numeric token.
