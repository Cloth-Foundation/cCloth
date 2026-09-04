# Proposal: Stage 33 numeric literal notation

Status: **complete — 33.4 exit audit passed 2026-09-04**.

This proposal adds scientific notation, explicit integer base prefixes, and
digit separators to Cloth's existing numeric literals. The notation changes
how a value may be written, not the primitive type system or numeric semantics.

See the [roadmap](../../ROADMAP.md#stage-33-numeric-literal-notation) and
[work ledger](../../TODO.md#stage-33-numeric-literal-notation).

## 1. Source syntax

All digits and punctuation in this contract are ASCII. Unary `+` and `-`
remain operators outside a literal; a sign immediately after `e` or `E` is
part of a decimal exponent.

### Decimal and scientific forms

A decimal digit run starts and ends with a digit and may contain one underscore
between adjacent digits. Existing integer and fractional forms remain valid.
Scientific notation adds `e` or `E`, an optional `+` or `-`, and a required
decimal digit run:

```cloth
var thousand = 1e3;
var fraction = 1.5e-2;
var precise = 6.022_140_76E23;
var compact = 1e3f32;
```

An exponent makes the literal floating even when the mantissa has no decimal
point. The existing decimal-point rule remains: both sides require a digit, so
`.5` and `1.` are not numeric literal forms. `e` and `E` are equivalent. Their
case has no connection to identifier visibility.

The exact mathematical value is the decimal mantissa multiplied by ten raised
to the signed exponent. It is rounded once under the existing target-type rule.
Finite-value, signed-zero, subnormal, and nonzero-underflow behavior remains
unchanged.

### Integer base prefixes

The new integer forms are:

| Prefix | Base | Digits |
| --- | --- | --- |
| `0b` | 2 | `0`–`1` |
| `0o` | 8 | `0`–`7` |
| `0x` | 16 | `0`–`9`, `a`–`f`, `A`–`F` |

Prefixes are lowercase and require at least one digit. Uppercase hexadecimal
digits are accepted because numeric spelling is independent of Cloth's
identifier-capitalization visibility rule. Leading zeroes alone do not select
a base: `012` remains decimal twelve.

```cloth
var mask = 0b1111_0000;
var mode = 0o755;
var color = 0xFF_80_00;
uint16 bits = 0xFFFFu16;
```

Base-prefixed literals are integer literals. They do not admit a decimal point,
an exponent, or a floating suffix. Unsuffixed forms retain the existing
contextual integer-literal behavior; canonical `i8` through `i64` and `u8`
through `u64` suffixes select the exact Stage 32 type.

Hexadecimal digits are consumed before suffix interpretation. Consequently,
`0x1f32` is the hexadecimal integer `0x1F32`, never a `float32` literal.
Because every character in `f32` and `f64` is a hexadecimal digit, a floating
suffix cannot be expressed after a hexadecimal core. Use a checked conversion
such as `float32(0x1F32)` when that conversion is intended. A complete `f32` or
`f64` tail on a binary or octal literal is diagnosed as a disallowed floating
suffix rather than accepted as a value.

### Digit separators

One underscore may separate two digits within any digit run: the decimal
integer part, fractional part, exponent, or base-specific digit sequence.
Underscores do not affect value or type.

```cloth
var population = 1_000_000;
var ratio = 12_345.67_89;
var scale = 1.25e1_0;
var flags = 0b1010_0101u8;
```

An underscore is invalid at the start or end of a run, next to another
underscore, next to a base prefix, decimal point, exponent marker or exponent
sign, or between a core and suffix. Therefore `1_`, `1__0`, `0x_FF`, `1_.0`,
`1.0_e2`, `1e_2`, `1e+_2`, and `1_i32` are invalid. Whitespace is never a
separator and terminates the numeric token.

## 2. Typing and values

Notation does not introduce a type or conversion. A base-prefixed literal is an
integer literal; an exponent form is a floating literal; separators are erased
before value interpretation. The Stage 20 contextual rules and Stage 32 exact
suffix rules then apply without modification.

An unconstrained, unsuffixed base integer defaults to `int32` when representable.
An unconstrained exponent form defaults to `float64`. Expected numeric types may
retarget an unsuffixed literal exactly as they do today. A suffix fixes the
initial type before existing widening, common-type, overload, switch, checked
conversion, `wrap`, and `sat` rules run.

Base-prefixed magnitudes use exact integer arithmetic and must fit the selected
type. The existing immediately applied unary-minus rule still admits signed
minima, including `-0x80i8`; unsigned suffixes still reject a negative value.
Scientific values use the existing deterministic IEEE-754 round-to-nearest,
ties-to-even contract. Host locale, host floating environment, and target
architecture cannot change accepted values or canonical bits.

No spelling denotes infinity or NaN. A nonzero scientific value that rounds to
zero remains an error, and a value that rounds to infinity remains an error.
Zero with an arbitrarily large syntactic exponent remains zero. Decoding must
classify extreme exponents without constructing storage proportional to the
numeric exponent.

## 3. Lexing, recovery, and diagnostics

A numeric token begins with an ASCII decimal digit. The lexer consumes the
complete numeric-shaped candidate, including a recognized base prefix,
separators, fraction, exponent and exponent sign, and suffix. An identifier-
continuation tail without intervening whitespace is part of the same candidate,
preserving Stage 32's no-splitting rule.

`+` or `-` belongs to the token only immediately after a decimal `e` or `E`.
A dot enters a decimal numeric candidate only under the existing rule that its
next character is a decimal digit; otherwise it remains member-access
punctuation. Operators, delimiters, comments, and whitespace otherwise end the
candidate.

Malformed candidates produce one primary numeric diagnostic and recover after
the complete candidate. Diagnostics distinguish at least:

- a missing digit after a base prefix or exponent;
- a digit outside the selected base;
- a noncanonical uppercase or unknown base prefix;
- invalid underscore placement;
- an integer suffix on a decimal floating core or a floating suffix on a
  nondecimal core;
- an unknown or noncanonical suffix; and
- a valid spelling whose value is outside its selected type.

Representative invalid atoms include `0b`, `0b102`, `0o8`, `0xG`, `0XFF`,
`1e`, `1e+`, `1e3i32`, `0b10f32`, `1__0`, and `0xFF_i8`. Invalid input is not
reinterpreted as several literals, identifiers, or declarations. Diagnostics
remain source ordered and deterministic.

The existing 4,096-byte required-constant limit counts the complete source
token, including prefixes, separators, exponent bytes, and suffix bytes.
Decoding is linear in token length and uses bounded exponent accumulation.

## 4. Compiler representation

The AST retains the complete source spelling and range. One shared spelling
decoder owns radix, separator, exponent, suffix, and error classification.
Semantic analysis resolves the existing exact `TypeId` and checks the value
before HIR lowering.

HIR never retains source notation:

- an integer literal stores the minimal base-ten magnitude, with no prefix,
  separators, suffix, or leading zeroes except the single spelling `0`; and
- a floating literal stores an exact normalized decimal magnitude as
  `coefficient` followed by lowercase `e` and a base-ten exponent. A nonzero
  coefficient has no leading or trailing zeroes; the exponent has no `+` or
  leading zeroes; zero is exactly `0e0`.

For example, `0x00_FFu16` lowers as integer magnitude `255` with type `uint16`;
`1.25e1_0f32` lowers as floating magnitude `125e8` with type `float32`; and
`0.0E999f64` lowers as `0e0` with type `float64`. Unary signs remain separate
HIR operations.

HIR verification accepts only those canonical forms and validates them against
the resolved type. Constant evaluation, MIR, optimization, artifacts, ABI
lowering, and LLVM consume exact existing types and canonical bits; none parse
source prefixes, separators, suffixes, or exponent case. Whole-project,
separate-package, and source-free compilation must therefore agree.

## 5. Compatibility and tooling

Stage 33 adds no keyword, primitive type, runtime symbol, target-specific
behavior, command-line option, capability, Shuttle manifest setting, receipt
field, cache-key field, or scheduling policy. Previously valid decimal and
typed literals keep their type and value.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and Shuttle manifest schema **1** remain unchanged.
The compiler executable digest supplies normal invalidation. Shuttle continues
to treat source and artifacts as opaque compiler data.

The VS Code grammar and user-facing numeric/syntax documentation change only
when the implementation checkpoint makes the syntax usable. The contract
checkpoint itself changes no compiler, runtime, Shuttle, editor, or artifact
implementation.

## 6. Verification requirements

Required coverage includes:

- lowercase base prefixes, uppercase/lowercase hexadecimal digits, leading
  zeroes, every radix boundary, and all Stage 32 integer suffixes;
- decimal scientific forms with `e`/`E`, signed exponents, integral/fractional
  mantissas, `f32`/`f64`, signed zero, ties, subnormals, finite extrema,
  underflow, overflow, and extreme exponent text;
- separators in every permitted digit run and every forbidden boundary;
- the hexadecimal `f32`/`f64` ambiguity and rejected nondecimal floating forms;
- atomic recovery for invalid prefixes, digits, exponents, separators,
  suffixes, and identifier tails in deterministic source order;
- unchanged unsuffixed defaults, contextual typing, exact suffix typing,
  widening, overloads, switches, conversions, `wrap`, and `sat`;
- canonical AST-to-HIR normalization, forged-HIR rejection, static constants,
  MIR folding, and x86-64/wasm32 LLVM verification before and after O2;
- whole-project, separate-package, and source-free equivalence, affected-only
  invalidation, failed-output preservation, and relocated serial/parallel
  determinism; and
- development/sanitizer, Rust/shared Shuttle, editor, formatting,
  documentation, link, and repository quality gates.

Tests extend existing targets and shared launchers. No alternate evaluator,
runtime parser, host-library fallback, or external dependency is added.

## 7. Checkpoints

1. **33.1 — Contract (complete):** freeze grammar, case rules, ambiguity,
   typing, exact values, recovery, canonical representation, compatibility,
   verification, and non-goals.
2. **33.2 — Frontend (complete):** implement the shared decoder, atomic lexing,
   syntax classification, semantic checks, canonical HIR lowering and
   verification, diagnostics, and focused unit/check coverage.
3. **33.3 — Integration (complete):** complete constants, MIR/optimizer/LLVM,
   native and package behavior, Shuttle verification, editor support, and user
   documentation.
4. **33.4 — Exit audit (complete):** close the complete boundary, malformed,
   exact-value, cross-target, package-determinism, and quality-gate matrices.

Stage 33 is complete following its separately authorized 33.4 exit audit on
2026-09-04.

## Non-goals

Stage 33 does not add hexadecimal floating-point or binary-exponent notation;
leading-dot or trailing-dot decimals; implicit octal; uppercase base prefixes;
new suffixes or types; a `byte` suffix; arbitrary-precision, decimal, half, or
complex primitives; units or user-defined suffixes; locale-specific digits;
implicit narrowing; new arithmetic or conversion behavior; wrapping or
saturating arithmetic; exceptions; optimizer controls; reflection of source
spelling; or unrelated language and tooling work.
