# Proposal: Stage 34 typed errors

Status: **complete — coordinated 34.4 exit audit passed 2026-09-05**.

This proposal gives Cloth typed exceptional completion without `try`, `catch`,
`recover`, checked call-site markers, or host-dependent exception handling. An
error is a normal GC-managed nominal object. A callable declares or infers the
error types that may leave it, calls use ordinary syntax, and an error that
reaches the application boundary is reported deterministically.

See the [roadmap](../../ROADMAP.md#stage-34-typed-errors) and
[work ledger](../../TODO.md#stage-34-typed-errors).

## 1. Language model

Cloth distinguishes three outcomes:

- a callable returns its declared success value;
- a nullable value represents expected absence in ordinary control flow; or
- a callable completes exceptionally with a non-null `Error` object.

Exceptional completion is a separate effect, not a nullable return, sentinel
value, hidden integer status, enum case, or source-visible result wrapper. A
throwing call either produces its success value or propagates its error. No
initializer, assignment, return, argument list, or later side effect observes a
success value on the error path.

Expected absence remains concise:

```cloth
var settings = TryLoadSettings() ?? DefaultSettings();
```

Absence may be promoted to a typed failure with a throw expression:

```cloth
var user = FindUser(id) ?? throw UserNotFound(id);
```

The left operand is evaluated once. The error constructor and `throw` operand
are evaluated only when the nullable value is null. On success the result has
the corresponding non-null type.

## 2. Error declarations and the universal root

`error` is a file-wide nominal type envelope. Like `class`, `struct`, `enum`,
and `interface`, it does not repeat the file stem:

```cloth
// UserNotFound.co
error {
  UserNotFound() {}

  UserNotFound(string message): Error(message) {}
}
```

An error is a managed reference type with ordinary class layout, fields,
functions, constructors, virtual dispatch, visibility, nullability, and GC
behavior. Construction remains `UserNotFound(...)`; Cloth does not add `new`.
Reference equality and casts retain the existing class rules.

The compiler provides one always-visible abstract `Error` root derived from
`object`. It cannot be shadowed, imported under a conflicting name, or
constructed directly. It provides a public final `string Message` field and
two base constructors:

- `Error()` initializes `Message` to the empty string; and
- `Error(string message)` initializes it to the supplied non-null string.

An error envelope without an explicit base derives directly from `Error`. A
constructor in such an envelope that omits a base initializer implicitly calls
`Error()`; this error-root rule is deliberate and does not change ordinary
class construction. An explicit message uses the ordinary
`: Error(message)` initializer.

An error may derive from one other error and may implement interfaces:

```cloth
// InvalidProfile.co
error : ValidationError is Renderable {
  InvalidProfile(string field): ValidationError(field) {}

  override func Render(): string {
    return Message;
  }
}
```

The direct base of an error must itself be an error. An ordinary class cannot
derive from `Error`, and an error cannot derive from an ordinary class. Error
inheritance is single and acyclic; interface conformance, abstract/sealed
modifiers, constructor visibility, overrides, and capitalization use the
existing class contracts. A declaration cannot combine `error` with another
file-type envelope.

`DivisionByZero` is a compiler-provided sealed error derived from `Error` and is
always visible. It has a public zero-argument constructor. No broader standard-
library distribution policy is implied by these two compiler-known types.

## 3. Throw expressions

`throw` is a lowercase keyword followed by one expression:

```cloth
throw InvalidProfile("Name");
```

The operand is evaluated exactly once and must have a non-null error type. A
nullable error, ordinary object, primitive, enum, struct, array, `void`, or null
literal is invalid. The statically resolved operand type is the thrown effect;
throwing a value typed as `Error` therefore contributes `Error`, even when its
runtime object is more specific.

A throw expression never produces a value. The compiler represents that fact
with an internal bottom type assignable to the surrounding expected type. Stage
34 does not introduce a source-visible `never` type. This permits `throw` in an
expression statement, return expression, argument, initializer, coalescing
right operand, or any other expression context whose evaluation reaches it.

In particular, `T? ?? throw E()` has type `T`, not `T?`. Existing `??`
precedence and lazy right-operand evaluation are unchanged; the form parses as
`nullable ?? (throw E())`. Applying `??` to a non-null value remains invalid,
and `??` never catches or converts an already thrown error.

Code following an unconditional throw is unreachable under the existing flow
rules. Earlier effects remain observable, while no later operand, store,
constructor publication, or statement executes.

## 4. Declared and inferred throws sets

`throws` follows a function's optional return type and precedes its body or
declaration semicolon:

```cloth
func Load(): object throws IoError, ParseError {
  return ReadSource();
}

func Save() throws IoError {
  WriteOutput();
}
```

It follows a constructor parameter list and precedes an optional base
initializer:

```cloth
User(string name) throws InvalidName: Human(name) {
  Name = ValidateName(name) ?? throw InvalidName(name);
}
```

The list is a semantic set of one or more visible, non-null error types.
Spelling order is retained for source diagnostics; canonical type identity
determines deterministic semantic and artifact order. Empty lists, duplicates,
inaccessible types, nullable types, non-error types, and a listed subtype
already covered by another listed base are invalid.

A listed base covers every derived error. `throws Error` therefore permits any
error. Declaring a conservative superset is valid even when the current body
does not produce every listed type. A callable with no declared or inferred
errors is nonthrowing.

Throws sets do not distinguish overloads and do not enter source-level callable
names. Parameter identity and the existing return rules continue to select a
callable. The set is nevertheless part of its public semantic contract and
package interface.

### Public and private callables

Public functions and public constructors must explicitly declare every error
type that may leave them. Omitting `throws` on a public callable declares an
empty set. A public contract may expose only accessible error types; it may use
an accessible base such as `Error` to cover a private derived error.

Private lowercase functions and private constructors may omit `throws`. The
compiler infers their minimal transitive set from reachable direct throws,
throwing calls, field initializers, and base construction. A private callable
may state an explicit `throws` set to constrain and document the same analysis.

Inference over recursive private call graphs is a deterministic fixed point.
Declaration or discovery order cannot change the inferred set or diagnostics.
An inferred private effect that reaches a public caller must be covered by that
public caller's explicit set.

### Calls and propagation

Calls retain ordinary syntax:

```cloth
func Build(): object throws SomeError {
  var value = MyFunc();
  return value;
}
```

There is no `try` marker at the call site. If `MyFunc()` succeeds, `value` is
initialized normally. If it throws, `value` is never initialized and the error
automatically leaves `Build()`. The call is invalid when a possible error is
not covered by the enclosing callable's declared or inferred set.

The same rule applies to arguments, local and instance-field initializers,
base-initializer arguments, constructor calls, member calls, static calls,
virtual dispatch, interface dispatch, and imported calls. An instance-field
initializer's effects contribute to every constructor that executes it. Static
final required constants remain compile-time expressions and cannot throw.

Every explicitly or implicitly invoked constructor contributes its complete
base, field-initializer, and body effect. If construction throws, no reference
to the partially initialized object is returned or stored. Existing roots keep
the allocation safe until the error path releases it; it then becomes ordinary
unreachable GC-managed storage.

## 5. Inheritance, interfaces, and entry points

An override or interface implementation may preserve or narrow the inherited
throws set. It cannot add an error not covered by the selected base or interface
contract. A nonthrowing implementation may satisfy a throwing contract. Static
receiver type determines the errors visible at a call; dynamic dispatch cannot
produce a wider set.

Identical inherited interface signatures merge only when their return and
throws contracts are compatible. The effective permitted set is their
intersection after subtype coverage: an implementation must satisfy every
inherited contract. An empty intersection requires a nonthrowing
implementation. Incompatible inaccessible identities are diagnosed rather than
silently widened to `Error`.

Abstract and interface declarations may state throws sets before their
semicolon. Existing `override`, `abstract override`, covariance, finality, and
visibility rules remain in force. Throws alone never create another overload or
virtual slot.

The entry point may declare a throws set:

```cloth
static func Main() throws Error {
  RunApplication();
}
```

The generated native entry wrapper is the only implicit consumer of an error.
It reports any verified error escaping `Main` and returns a nonzero process
status. A nonthrowing `Main` retains its current signature and behavior.

## 6. Runtime failure migration

Executed integer division or remainder with a zero divisor completes
exceptionally with `DivisionByZero`. The operation
has that throws effect unless semantic constant evaluation proves its divisor
nonzero independently of optimization. Both `/` and `%` use the same error
type. Floating division retains IEEE behavior.

A required compile-time constant that divides or remainders by zero remains a
compile-time diagnostic and never becomes a runtime error. Stage 31 optimization
may preserve or eliminate an unreachable error edge but cannot change whether
valid source requires a throws contract.

Signed minimum divided or remaindered by `-1`, checked addition/subtraction/
multiplication/negation overflow, and invalid shift counts retain their Stage 29
and Stage 21 terminal-failure behavior. Stage 34 does not silently migrate every
runtime trap into the error model.

## 7. Compiler representation and portable ABI

The AST retains error envelopes, source-order throws lists, and throw-expression
ranges. Semantic analysis resolves error identities, computes effect sets, and
performs flow checking before HIR lowering. HIR carries canonical error types
and an explicit throw expression; MIR represents exceptional completion and
the success/error edges of throwing calls. Verifiers reject missing, extra,
inaccessible, non-error, noncanonical, or ABI-incompatible effects.

Cloth does not lower errors through C++ exceptions, LLVM personality functions,
platform unwinding, `setjmp`/`longjmp`, thread-local status, or a sentinel success
value. A throwing callable uses an explicit target-neutral error-return ABI:

- the physical function returns a nullable managed `Error` reference;
- a non-void success value is written through a compiler-owned first result
  out-parameter;
- null error means the result out-parameter was initialized successfully; and
- non-null error means the result out-parameter was not written.

A throwing `void` callable returns only the nullable error reference. A
throwing allocation wrapper receives an object-reference result out-parameter;
its initializer operates on the rooted allocation and publishes it only after
success. Propagation checks the returned error exactly once, transfers it to a
caller root, performs required root-frame cleanup, and returns it without
executing the success continuation.

Nonthrowing callables retain their existing physical return ABI. A virtual or
interface slot declared throwing retains the throwing physical ABI even when an
implementation narrows its effect set to empty. This keeps every dynamic target
ABI-compatible. Direct and virtual calls must agree with the verified slot
contract rather than guessing from a body.

The optimizer treats throw and error propagation as observable control flow. It
cannot move stores or side effects across a possible error, manufacture a
success value on an error path, discard a reachable error, or replace one error
identity with another. LLVM verification is required before and after O2 for
x86-64 and wasm32.

## 8. Packages and compatibility

Package interfaces must encode:

- the error kind and its canonical error base;
- inherited `Error` layout and public error declarations;
- each public callable's canonical throws set; and
- whether each callable or virtual/interface slot uses the throwing ABI.

Source-free consumers perform the same type, call, override, interface, and
propagation checks as whole-project consumers. Error identities use existing
canonical package/type identity. Private inferred effects do not become public
unless covered by an exported callable contract.

The implementation checkpoints target [artifact format **5**](../artifact_schema_v5.md), compiler ABI
**5**, and runtime ABI **4**. Format 5 carries error kinds and throws sets;
compiler ABI 5 defines the result/error calling convention; runtime ABI 4 adds
the compiler-known descriptors and terminal reporter. Older artifacts are
rejected and rebuilt rather than adapted or guessed.

Process protocol **2**, receipt schema **1**, and Shuttle manifest schema **1**
remain unchanged. Shuttle treats error metadata and object code as opaque
compiler artifacts. Compiler/runtime digests and compatibility queries provide
normal invalidation; no manifest option, cache policy, scheduler rule, or
Shuttle-side source parsing is added.

Checkpoint 34.3 activates the coordinated MIR, artifact, ABI, runtime, native,
package, Shuttle, editor, and user-documentation integration. The repository's
active constants are artifact format 5, compiler ABI 5, and runtime ABI 4.

## 9. Diagnostics and terminal reporting

Diagnostics must identify the precise source error and callable. Required
categories include invalid error bases, invalid throw operands, malformed or
redundant throws lists, inaccessible public error contracts, uncovered direct
or transitive effects, widening overrides, incompatible interface contracts,
constructor/field/base effects, and invalid `Main` declarations. Independent
errors remain source ordered; one bad effect must not produce verifier or
lowering cascades.

An error escaping `Main` writes to standard error and leaves standard output
unchanged. The stable form is:

```text
cloth error: <CanonicalTypeName>
```

When `Message` is non-empty, the runtime appends `: ` and the exact UTF-8 message
before the final newline. The runtime returns a nonzero process status and does
not print an implementation address, allocation identity, host exception name,
or stack trace. An error message may contain its own control characters; its
bytes are not locale-transcoded or rewritten.

The compiler frontend recognizes `error`, `throw`, and `throws` at 34.2. The VS
Code grammar and user documentation change with the integrated native behavior
in 34.3, not with the frontend-only checkpoint.

## 10. Verification requirements

Required coverage includes:

- direct and derived error declarations, abstract/sealed errors, interfaces,
  fields, both `Error` base constructors, visibility, construction, casts,
  nullability, identity, and invalid class/error inheritance combinations;
- throw expressions as statements, returns, arguments, locals, fields,
  constructor/base arguments, and lazy `??` operands, including exact once-only
  evaluation, bottom typing, non-null narrowing, and unreachable flow;
- zero, one, multiple, base-covered, duplicate, redundant, nullable,
  inaccessible, and non-error throws-list entries;
- explicit public contracts, inferred and constrained private contracts,
  recursive fixed points, imported calls, overloads, virtual/interface calls,
  narrowing implementations, widening rejection, and `Main`;
- successful and failing ordinary/object/array/enum/struct/void returns, result
  out-parameter nonpublication, failed construction, GC-root transfer, and no
  side effect or store after propagation;
- explicit throw identity and message, `DivisionByZero` for integer `/` and `%`,
  compile-time zero diagnostics, proven nonzero divisors, unchanged floating
  behavior, and unchanged remaining terminal traps;
- AST/HIR/MIR canonicalization and forged-state rejection, optimizer safety,
  x86-64/wasm32 LLVM verification before and after O2, and native terminal
  output/status;
- format-5/compiler-ABI-5/runtime-ABI-4 readers, writers, fixtures, stale
  rejection, capability reporting, whole/separate/source-free equivalence,
  affected-only invalidation, failed-output preservation, and relocated
  serial/parallel determinism; and
- development/sanitizer suites, Rust/shared Shuttle tests, editor tests,
  formatting, user and maintainer documentation, local links, and repository
  quality gates.

## 11. Checkpoints

1. **34.1 — Contract (complete):** freeze error types, the universal root,
   throw expressions, throws sets, inference, calls, construction, inheritance,
   interfaces, division-by-zero migration, portable propagation ABI,
   compatibility transition, diagnostics, tests, and non-goals.
2. **34.2 — Frontend and interfaces (complete):** implement keywords, parsing,
   AST, semantic error/effect analysis, private fixed-point inference,
   null-flow/bottom typing, HIR, verifier coverage, and focused frontend checks.
3. **34.3 — Lowering and integration (complete):** implement MIR error edges,
   the result/error ABI, GC-safe propagation, compiler-known error descriptors,
   runtime reporting, division/remainder migration, compatibility formats,
   LLVM/native/package/Shuttle behavior, editor support, and user documentation.
4. **34.4 — Exit audit (complete):** close the complete declaration, effect,
   construction, propagation, runtime-failure, malformed-state, cross-target,
   package-determinism, and quality-gate matrices.

Stage 34 is complete. The audit added explicit user-error import coverage,
constructor bottom-flow validation, forged MIR/ABI rejection, runtime error
descriptor and tracing checks, all supported result shapes, failed-constructor
execution, and typed-error package invalidation and output preservation. Both
compiler configurations and every coordinated toolchain gate pass.

## Non-goals

Stage 34 does not add `try`, `catch`, `recover`, `finally`, local error handling,
error suppression, catch filters, multi-catch, rethrow syntax, resumable errors,
stack traces, source locations in runtime reports, `defer`, deterministic
resource disposal, payload enums, union/result types, generics, a
source-visible `never` type, async cancellation, foreign C++/SEH/signal
interoperation, user-defined effect kinds, dynamic static initialization, a
general standard-library distribution system, conversion of every runtime trap
to an error, or unrelated language/tooling work.
