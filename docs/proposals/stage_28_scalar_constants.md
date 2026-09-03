# Proposal: Stage 28 compile-time scalar constants

Status: **complete — approved and verified through 28.4 on 2026-09-02**.

The user approved the concrete source, evaluation, resource, and artifact-format
contracts below on 2026-09-02 and separately authorized 28.2, 28.3, and 28.4.
Typed evaluation and native/package integration are implemented and verified.
Artifact format 4 is required, with the other compatibility versions unchanged.
The [28.4 exit audit](../testing.md#stage-284-scalar-constant-exit-audit) closes
the compiler and Shuttle work ledgers; no later stage is activated.

See the [roadmap](../../ROADMAP.md#stage-28-compile-time-scalar-constants) and
[work ledger](../../TODO.md#stage-28-compile-time-scalar-constants).

## Objective and boundaries

Give every supported `static final` scalar field one verified compile-time
value, shared by semantic checking, switch labels, HIR/MIR, native emission,
and package artifacts. Permit bounded, side-effect-free scalar expressions and
references to other constants without creating a runtime initialization order.

This closes the recorded unary-initializer gap: before this stage,
`static final int8 Minimum = int8(-128);` could pass frontend checks without
a value that all later boundaries could retain. Integer/enum symbols retained
selected values, whereas static emission/export recovered values from returned
MIR literals. The evaluator now retains all supported scalar values; 28.3 removes
output-side literal recovery and carries verified bits through every boundary.

Constant evaluation is required language validation, independent of optimization
settings. It does not fold arbitrary function bodies, infer new return-flow
facts, or change which ordinary locals are constants. Future optimizer folding
must not apply constant-declaration diagnostics to ordinary runtime expressions.

## 1. Source surface

Keep `static final`, existing type names, operators, and access syntax. Add no
keyword, attribute, numeric suffix, or compile-time function syntax.

```cloth
// Limits.co
static final int32 BufferSize = 4 * 1024;
static final int32 LastIndex = BufferSize - 1;
static final int8 Minimum = int8(-128);
static final bool Enabled = BufferSize > 0 && LastIndex < BufferSize;
static final float32 Third = 1.0 / 3.0;

// With Status in scope:
static final Status Initial = Status.Pending;
static final Status Fallback = Initial;
```

Supported result types remain `bool`, `char`, `byte`, fixed-width signed and
unsigned integers, `float32`, `float64`, and nominal enums. Aliases retain their
meanings: `int` is `int32`, `uint` is `uint32`, and `float` is `float32`.
No inferred field type or new field-owning type is introduced. Enums themselves
still cannot declare ordinary fields or functions.

String, object, class, interface, array, nullable, struct, and `void` values are
outside this constant domain. Static fields still require both `final` and an
initializer, retain ordinary capitalization-based visibility, occupy no instance
storage, and cannot be accessed through objects.

### Permitted expressions

Every operand must itself be an eligible scalar constant expression:

| Form | Contract |
| --- | --- |
| Scalar literal; parentheses | Existing literal spellings, escapes, and typing |
| Qualified enum case | Exact nominal enum identity; all cases remain public |
| Static constant reference | Existing unqualified same-file or qualified type lookup, including imports/aliases and existing inherited lookup |
| Prefix `+`, `-` | Numeric operands only |
| Prefix `~` | Fixed-width integer complement |
| Prefix `!` | Boolean negation only |
| `+`, `-`, `*`, `/` | Numeric operands with ordinary common-type rules |
| `%`, `&`, `\|`, `^`, `<<`, `>>` | Existing integer-only rules |
| `==`, `!=` | Compatible numeric values, two booleans, two characters, or the same nominal enum |
| `<`, `<=`, `>`, `>=` | Numeric operands only |
| `&&`, `\|\|` | Boolean operands, with short-circuit evaluation |
| `NumericType(expression)` | Existing checked built-in numeric conversion, not a function call |

Reject function/constructor/intrinsic calls, member reads through instances,
locals and parameters (including `final` locals), assignments, updates, indexing,
array/struct construction, string operations, meta operations, `is`, `as`, and
null operations. Purity is not inferred from a function body. No expression is
eligible merely because an optimizer could compute its value.

Character literals keep their current byte-oriented decoding and existing
artifact range `0..255`; character values still have `char` type and 32-bit
storage. This stage adds constant references/equality, not new Unicode escapes,
numeric character conversions, or wider character-literal support.

## 2. Typing and numeric evaluation

Bind and type expressions before evaluating them. Reuse
[numeric literal/conversion rules](../numeric_conversions.md) and
[integer operator rules](../integer_binary_data.md), including contextual
literals, lossless widening, common operand types, and distinct `byte` identity.
A constant reference is a typed value, not a literal that can be retyped to fit.

```cloth
static final int32 Wide = 12;
static final int8 Invalid = Wide;       // Narrowing remains invalid.
static final int8 Narrow = int8(Wide);  // Checked conversion, computed now.
```

Evaluate at each expression's resolved type, not with unlimited mathematical
precision followed by a single final conversion. A wider destination only
affects operands where the existing contextual-typing rules permit it. Preserve
left-to-right operand evaluation, grouping, and conversion placement; do not
reassociate expressions or implicitly fuse floating operations.

### Integer policy

In a required constant initializer, arithmetic overflow is a diagnostic:

- `+`, `-`, `*`, and unary `-` require their mathematical result to fit the
  resolved integer type. Negating a nonzero unsigned value or a signed minimum
  is invalid. Existing signed-literal handling still admits `-128` as `int8`
  and `-9223372036854775808` as `int64`. The innermost sign forms a literal;
  each outer sign is an evaluated unary operation, including inside a literal
  conversion. Do not cancel signs and conceal an intermediate overflow.
- Division truncates toward zero. Remainder has the dividend's sign when
  nonzero. Reject a zero divisor and signed-minimum divided or remaindered by
  `-1`, before performing the operation; no host/LLVM undefined case is used.
- Bitwise operations act on exactly the declared width. Left shift discards
  high bits; signed right shift extends the sign and unsigned right shift
  fills with zero. These are bit operations, not checked multiplication.
- Shift counts must be in `[0, left_operand_width)`, regardless of the count's
  integer type. Retain existing early literal-count diagnostics; additionally
  validate evaluated constant counts.

This is an explicit constant-context restriction, not a new runtime overflow
policy. Current runtime addition/subtraction/multiplication use fixed-width LLVM
operations without overflow guards, and division/remainder are emitted directly.
Runtime failure-policy work is separately deferred. An optimizer must preserve
runtime behavior rather than reuse this evaluator's diagnostic policy blindly.

### Floating-point policy

Use IEEE-754 binary32/binary64, rounding to nearest with ties to even at each
typed operation. Preserve signed zero and gradual underflow. No host extended
precision, ambient rounding mode, locale, fast-math assumptions, fused multiply-add,
or host-dependent decimal round trip may determine the result.

Every evaluated floating operand/result must be finite, not merely the final
field value. Arithmetic producing infinity or NaN is a constant-evaluation
error; this includes division by either signed zero. Finite subnormals and
arithmetic/conversion underflow to signed zero are valid. This finite-only domain
matches the existing artifact restriction and does not restrict runtime floats.

Decimal literals retain destination-directed rounding and the existing literal
fit distinction: a nonzero literal that underflows to zero is out of range,
whereas underflow during an evaluated operation or explicit typed-value
conversion is accepted. No exponent or other new source spelling is added.

### Explicit numeric conversions

Retain the distinction between a numeric literal expression and an already
typed value. Literal conversions use existing destination-directed checking;
making a reference constant does not turn it into a literal.

- Integer-to-integer: require the mathematical value to fit; never wrap.
- Floating-to-integer: truncate toward zero, then check the integer range.
- Integer-to-floating: round to nearest, ties to even.
- `float32`-to-`float64`: widen exactly.
- Typed `float64`-to-`float32`: preserve the existing finite-source range guard,
  `-max_float32 <= value <= max_float32`, then round once. Precision loss and
  underflow are accepted. Destination-directed literal conversion retains its
  existing literal-fit rule, rather than first introducing a `float64` value.
- Identity conversion: retain the value, including a floating zero's sign.

An evaluated conversion failure is a source diagnostic in this context, never
a deferred startup trap. Booleans, characters, and enums gain no numeric casts.
Use deterministic integer/IEEE arithmetic facilities; C++ signed overflow,
out-of-range casts, and host floating operations are not the semantic authority.
Selection of an implementation library must respect the project's build and
licensing requirements; this proposal does not add a dependency or execute a JIT.

## 3. Eligibility, short-circuiting, and dependencies

Register member signatures first, then bind/type all constant initializers and
collect their constant-reference edges. Forward references within a file and
across files are valid when normal visibility/import rules permit them.
Import cycles between source files do not themselves imply constant cycles.
Shuttle's prohibition on package-dependency cycles is unchanged.

Eligibility, name resolution, typing, literal-fit checks, and dependency-cycle
checks examine the whole expression, including both sides of `&&` and `||`.
Value evaluation alone short-circuits:

```cloth
static final bool Safe = false && (1 / 0 == 0); // Valid: RHS is not evaluated.
static final bool Invalid = false && Probe();  // Invalid: calls are ineligible.
static final bool Cycle = false && Cycle;      // Invalid: a syntactic self-edge.
```

The example assumes `Probe` resolves to a boolean-returning function; its purity
or result is irrelevant. An invalid literal conversion on a skipped side still
fails ordinary typing. Referencing a separately invalid constant on a skipped
side does not make that declaration valid.

Evaluate every declared static constant, including unused/private ones, once
its dependencies are valid. Detect cycles from all syntactic constant-reference
edges, even skipped ones. Use memoized states that distinguish unevaluated,
evaluating, valid, and failed declarations; failure cannot become a zero value.
Use iterative graph traversal, not C++ recursion proportional to dependency depth.

Order roots and edges by canonical declaration identity, retaining source ranges
for diagnostics. Report a deterministic cycle with reference-site notes; bound
displayed notes to eight plus an omitted-count summary. Suppress repetitive
dependent-value errors after reporting the originating invalid declaration.
Independent failures must still be reported in stable order.

Imported constants are already verified typed values, not ASTs to reevaluate.
Private constants may contribute to an accessible public constant in their owner;
consumers may use that public value but gain no access to the private member.
No implicit transitive imports or weakened owner/visibility checks are introduced.

## 4. Compiler integration and switch contract

Retain one canonical typed scalar representation for all supported types:

- integers: zero-extended fixed-width bits, interpreted with the declared type;
- booleans: exactly zero or one;
- characters: decoded value in the currently supported range;
- floats: exact binary32/binary64 bits, finite and with signed zero preserved;
- enums: canonical nominal type plus a verified declaration-order tag.

Keep source expression/type/range information for diagnostics and verification,
but do not rediscover static values by searching arbitrary initializer MIR for
a returned literal. Semantic evaluation, HIR/MIR lowering, LLVM constants, and
artifact writers/readers must agree on this representation. No startup callable,
heap allocation, GC root, or executable expression is emitted for a constant.
Keep static global symbols, ownership, linkage, and physical types unchanged.

Independent HIR/MIR/imported-model verification must reject missing, wrong-type,
out-of-range, non-finite, wrong-owner, or inconsistent constant claims before
emission. Imported declaration claims must still agree with their verified owner
and exact dependency digest. Reject forged enum tags even when their integer
bits fit, and do not admit floats/bools/chars as MIR switch labels.

Stage 27's source label grammar and eligibility remain unchanged. A field whose
initializer uses new expressions can now supply its verified integer/enum value:

```cloth
switch (code) {
  case Limits.LastIndex: { println("last"); }
  default: { println("other"); }
}
```

Inline `case 4 * 1024:`, `case int8(Wide):`, and boolean/float/string selectors
remain invalid. Existing contextual integer labels, widening, duplicate
normalization, enum identity/exhaustiveness, scopes, and transfers are unchanged.
Reading a constant elsewhere does not add local constant propagation, new
smart casts, return completeness, or constant-driven dead-code elimination.

The 28.2 checkpoint exposed the new semantics through direct `--check` only.
Stage 28.3 removes that temporary gate: native/IR/package emission consumes the
same verified values. Invalid constants must still preserve completed outputs.

## 5. Approved artifact transition and Shuttle boundary

An artifact revision is necessary: format 3 inherits a nonnegative-only integer
constant encoding and its reader rejects negative signed values. Relaxing that
reader under the same frozen format would silently change the accepted schema.

The approved transition is **artifact format 4**, retaining compiler ABI **4**
(`_C4` names), runtime ABI **2**, process protocol **2**, receipt schema **1**,
and manifest schema **1**.
There is no calling-convention, object-layout, or runtime-service change.

Format 4 inherits format 3 except for the envelope format integer and this scalar
rule: `static_value` with `kind: "integer"` contains a canonical decimal string
of the mathematical value. Signed types admit their full negative and positive
ranges; unsigned types admit only their existing nonnegative ranges. Reject
leading `+`, leading zeros except `"0"`, `"-0"`, whitespace, fractions, and
out-of-range values. No shape/key changes or expression/dependency AST records
are introduced. Boolean, character, float-bit, and enum encodings stay unchanged.

Review vectors below are metadata examples, not loadable artifacts. The declared
type comes from the owning member record; JSON string quotes are shown explicitly.

| Declared type/value | `kind` | `value` |
| --- | --- | --- |
| `int8` minimum | `integer` | `"-128"` |
| `int64` minimum | `integer` | `"-9223372036854775808"` |
| `uint64` maximum | `integer` | `"18446744073709551615"` |
| integer zero | `integer` | `"0"` |
| `bool` true | `boolean` | `true` |
| `char` newline | `character` | `"10"` |
| `float32` negative zero | `float32` | `"80000000"` |
| `float64` negative zero | `float64` | `"8000000000000000"` |
| `float32` result of `1.0 / 3.0` | `float32` | `"3eaaaaab"` |
| enum's second case | `enum` | `"1"`, validated against that exact enum |

Readers/writers, capabilities/receipts, Shuttle format validation, fixed golden
fixtures, compatibility diagnostics, and owning schema docs must transition
together in 28.3. Reject earlier formats and rebuild packages; do not reinterpret
or migrate them. Exact compiler identity still invalidates cached products.
Do not publish format 4 or modify current schema/version constants during 28.1.
Any deviation from this reviewed schema or physical ABI requires approval first.

Shuttle continues to treat artifacts as opaque. It must neither evaluate Cloth
expressions nor inspect their scalar metadata. Existing package digests retain
all dependency inputs; computed constants do not justify dropping an import or
weakening freshness checks. Value/source edits rebuild affected consumers under
the existing conservative reuse contract; equal values do not guarantee reuse.

## 6. Resource and diagnostic contract

Approved deterministic limits, checked before unsafe recursive traversal or
unbounded allocation:

- 65,536 locally declared static constants per source package;
- 65,536 source expression nodes per constant initializer;
- 1,048,576 source expression nodes across a package's static initializers;
- expression nesting depth 256, counting the root as one; and
- 4,096 bytes per numeric literal spelling in a constant initializer.

Count parentheses and skipped operands before evaluation; synthesized implicit
conversions do not change source counts. Constant references count as expression
nodes, bounding graph edges by total nodes. A long constant-dependency chain does
not consume the expression nesting budget; iterative evaluation must handle the
maximum declaration count. Memoization must prevent exponential diamond expansion.

Apply budgets per source package, not per invocation, so whole-project and
separate compilation have the same acceptance boundary. Imported values remain
subject to artifact limits and per-package constant-count validation, but consumers
do not replay unavailable initializer trees. Existing envelope/metadata limits
remain in force. Parser, typing, evaluator, and verifier traversal of required
constant expressions must enforce these budgets before recursion can exhaust the
host stack; an evaluator-only check is insufficient.

Distinguish ineligible syntax, ordinary type errors, evaluation failures, cycles,
resource limits, and malformed imported values. Point to the failing expression
and give declaration/reference context without dumping unbounded graphs. Source
errors use the existing status-one contract; invalid requests/artifacts retain
their existing status-two classification and typed resource-limit issue codes.
Neither frontend nor downstream failures may replace a completed output or cause
Shuttle to execute a stale program. No wall-clock evaluation budget is introduced.

## 7. Stages and exit gates

1. **28.1 — Contract:** review this source/evaluation/format proposal, record
   approval and non-goals, and synchronize both roadmaps/work ledgers. Completed
   by user approval on 2026-09-02; this does not itself authorize 28.2.
2. **28.2 — Typed evaluation (complete):** implement eligibility, ordinary typing integration,
   canonical scalar values, checked evaluation, forward references, cycle/error
   handling, and deterministic limits. Fix unary static initializers. Add unit
   and direct-check tests; retain the explicit temporary emission boundary above.
3. **28.3 — Integration (complete):** connect HIR/MIR verification, native globals, switch
   constants, imported values, and format-4 tooling as one coordinated transition.
   Remove the temporary gate only with whole/separate/source-free equivalence.
4. **28.4 — Exit audit (complete):** complete resource, malformed-model/artifact, dependency
   evolution, deterministic build, output-preservation, and native tests. Update
   user documentation, maintainer contracts, and editor support where affected;
   pass both compiler configurations, Rust, shared-toolchain/native, and editor gates.

Required coverage:

- every supported scalar/operator/conversion, integer endpoints and overflow,
  signed division/remainder edges, shifts and complement, and literal versus
  typed-constant conversions, including `int8(-128)` and full-width `uint64`;
- binary32/binary64 bit-exact arithmetic/rounding/conversion vectors, signed
  zero, subnormals, underflow distinctions, non-finite intermediates, and checked
  float-to-integer/narrowing boundaries without host-double test oracles;
- forward references, local/cross-file/unused/private cycles, skipped branches,
  forbidden calls on skipped branches, import aliases, privacy, and enum identity;
- maximum and one-over limits, long chains, broad diamonds, canonical diagnostics
  under reversed source registration, and no default values after failures;
- malformed typed constants and re-signed artifacts: invalid encodings, ranges,
  kinds/types, tags/owners, floats, counts, stale dependency claims, and old formats;
- x86-64/wasm32 value-bit agreement, LLVM verification, native agreement with
  ordinary runtime expressions for accepted constants, and no startup/GC work;
- source-free negative and computed constants, cross-package chains, constant
  edits that change switch coverage or create duplicates, consumer invalidation,
  stale-link rejection, and failed-output preservation; and
- relocated serial/parallel builds with reordered dependencies: equal artifacts
  within each target, equivalent whole/separate native output, and all preexisting
  scalar/enum/struct/switch/GC regressions. Cross-target artifacts themselves need
  not be byte-identical because their target/layout metadata differs.

## Non-goals and follow-on prerequisites

No mutable/reference static storage, dynamic initialization, startup ordering,
compile-time user functions, aggregate constants, enum metadata/payloads,
first-class constant locals, inline switch-expression labels, new scalar/character
syntax, runtime arithmetic-policy changes, wrapping/saturating conversions,
general optimizer folding, new targets, or unrelated Shuttle/editor features.

Struct-backed enum metadata still needs aggregate constant construction and its
own approved source contract; scalar evaluation alone does not activate it.
General optimization remains a separate stage. The stage closes only when its
entire scope and both repositories' exit gates are complete.
