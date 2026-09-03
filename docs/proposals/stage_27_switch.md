# Proposal: Stage 27 switch statements and exhaustive enum handling

Status: **complete — approved and verified through 27.4 on 2026-09-02**.

This is the approved implementation contract, not the user-facing language reference.
The user approved the concrete contract below and implementation through 27.4.
See the [roadmap](../../ROADMAP.md#stage-27-switch-statements-and-exhaustive-enum-handling)
and [work ledger](../../TODO.md#stage-27-switch-statements-and-exhaustive-enum-handling).
The [exit audit](../testing.md#stage-274-switch-exit-audit) records the completed
verification and unchanged compatibility versions.

## Objective and boundaries

Add structured selection over existing enum and integer values. Enum switches
make unhandled states a compile-time error, including when a dependency changes
its case set. Preserve exactly-once evaluation, nominal types, initialization,
nullable-flow facts, GC safety, and deterministic separate compilation.

This stage does not add pattern matching, destructuring, guards, ranges,
fallthrough, labeled jumps, switch expressions, or enum payloads. It does not
add general constant folding, uninitialized-local assignment support, new
selector types, or a runtime/target/distribution feature.

## Source syntax

Use familiar case labels with an explicit block for each arm:

```cloth
// State.co
enum { Ready, Running, Complete }

// Describe.co
static func Describe(State state): string {
  switch (state) {
    case State.Ready: {
      return "ready";
    }
    case State.Running: {
      return "running";
    }
    case State.Complete: {
      return "complete";
    }
  }
}
```

Braces are required for the switch and every arm, consistent with Cloth's
existing structured statements. The selector is parenthesized. An arm is not
a label attached to arbitrary following statements, and there is no shared
C-style switch-body scope. Completing an arm continues after the switch.

Several values may select one block without stacking empty case labels:

```cloth
switch (code) {
  case 0, 1: {
    println("accepted");
  }
  default: {
    println("other");
  }
}
```

Grammar, reusing the existing `expression` and `block` productions:

```ebnf
switch_statement = "switch" "(" expression ")" "{"
                   { switch_case } [ switch_default ] "}" ;
switch_case      = "case" expression { "," expression } ":" block ;
switch_default   = "default" ":" block ;
```

At least one arm is required. A default-only switch is valid and still evaluates
its selector. There is at most one `default`, and it must be last. A trailing
comma in a case-value list is not accepted. Labels are parsed as expressions
for ordinary name resolution and recovery, but only the constant forms below
are legal. `case` and `default` are invalid outside a switch's arm list.

`switch`, `case`, and `default` are reserved keywords. Declarations or dependency
aliases using those formerly ordinary identifiers must be renamed.
The already reserved `match` remains unsupported;
it is not an alternative spelling. Do not add a fallthrough keyword.

## Selector and case types

The selector must have an enum type or an existing fixed-width integer type:
`int8` through `int64`, `uint8` through `uint64`, or `byte`. The `int` and `uint`
aliases retain their existing meanings. There is no integer promotion merely
for switching. Booleans, characters, floats, strings, references, structs,
arrays, nullable values, and void expressions are rejected. `bool` and `char`
remain distinct from integers.

Permitted case values are:

- An integer literal, optionally negated or parenthesized, with contextual
  typing from the selector. Use existing literal spelling and representability
  rules, including signed minima, full-width unsigned values, and rejection of
  negative literals for unsigned types. This adds no literal syntax.
- An accessible enum case of the selector's exact canonical enum type, such as
  `State.Ready`. Type import aliases work normally; bare enum cases are not
  injected into scope. Case capitalization never restricts access.
- An accessible `static final` integer or enum field whose value is already
  retained as a verified scalar constant by the existing compiler/artifact
  model. Ordinary qualified or same-file unqualified lookup applies.

Parentheses may surround any permitted form. Negation is accepted only for an
integer literal, not an arbitrary constant expression. Ordinary locals and
parameters, including final locals, calls, conversions, arithmetic, meta
operations, assignments, and runtime field reads are not case constants.
No case expression executes at runtime.

A typed integer constant must have the selector type or widen to it under the
existing lossless integer rules. A typed constant does not gain literal-style
narrowing merely because its value would fit. Enum constants require exact
nominal identity; neither equal names nor equal internal tags make two enum
types interchangeable. Enums are never converted to integers for source checks.

Normalize accepted values to the selector type before detecting duplicates.
Aliases of the same enum case, a static constant naming that case, equivalent
integer spellings, and signed `-0`/`0` cannot create distinct labels. Duplicates
are errors both within one group and across arms, with a note at the first
label. Check every label; there is no first-match ambiguity to resolve.

## Exhaustiveness and evolution

An enum switch must either list every case of its enum exactly once or include
`default`. This rule applies in void functions and constructors too, not only
when return analysis needs exhaustiveness. Diagnose missing cases in enum
declaration order, bounding long lists with an omitted-case count.

`default` explicitly handles every otherwise-unlisted valid value. It may be
present even when all current enum cases are listed, allowing an intentional
fallback for future additions. It is not a way to admit an invalid enum tag.

An integer switch need not have a default. If no case matches, execution
continues after the switch. For flow analysis, an integer switch without
default always retains this no-match path, even if all values of a narrow
integer type happen to be enumerated. Integer-domain exhaustiveness inference
and constant-selector branch pruning are outside this stage.

Adding a dependency enum case causes an exhaustive consumer without default to
fail on recompilation until it handles that case. A consumer with default keeps
its declared fallback. Removed or renamed cases produce ordinary lookup errors.
Changing case order or scalar constant values must invalidate affected consumers;
no previously compiled switch may silently use stale tags or labels.

## Scope, evaluation, and transfers

Evaluate the selector exactly once before selecting an arm. Calls, updates,
array bounds checks, and other selector effects retain their existing order and
failure behavior. Capture its value: writes to the original variable inside an
arm do not restart selection or alter which arm is running. Only the selected
body executes.

Each arm has an independent lexical block. It may shadow outer locals under
existing rules; sibling arms may reuse local names, but cannot access one
another's locals. Case labels resolve outside their body scope. Declarations
cannot appear between arms or before the first arm.

- Arm completion exits the switch automatically; a trailing `break` is optional.
- `break;` exits the nearest enclosing loop or switch. It can exit an arm early.
- `continue;` targets the nearest enclosing loop, ignoring intervening switches.
  A switch alone does not make `continue` valid.
- In a classical `for`, continue still executes the updates; in array iteration
  it reaches the increment latch; in `while` it rechecks the condition.
- A loop nested inside an arm retains its own break/continue targets. A nested
  switch captures break but never continue. There are no labeled transfers.
- `return` still exits the enclosing function or constructor, not the switch.

Flow analysis must distinguish transfer targets. A break consumed by a switch
inside `while (true)` must not make the enclosing infinite loop appear to exit.
Conversely, a continue crossing a switch must not become an arm fallthrough.

## Return paths, initialization, and narrowing

Analyze all supplied arms, without pruning based on a constant selector. An
explicit default participates in conservative flow checking even when every
current enum value also has a named label; do not warn merely for keeping such
a fallback. An exhaustive enum switch without default has no valid-value
no-match path. An integer switch without default does.

A switch can fall through when an arm reaches its end, a break targets that
switch, or a no-match path exists. If every possible arm returns or otherwise
cannot reach the join, it cannot fall through. Non-void function completeness
and existing unreachable-statement warnings use those facts.

Merge initialization and non-null facts from paths that actually reach each
destination, including early switch breaks and the integer no-match path.
Returns and loop continues do not contribute to the switch join; constructor
returns still need the existing required-field checks. Statements after a
transfer cannot manufacture initialization or narrowing facts.

Required class/struct/enum fields and final fields keep their current rules:
every constructor exit must be initialized, final initialization must be exactly
once by a direct assignment, and reads or self escape before initialization
remain invalid. A switch inside a loop does not relax the existing prohibition
on final-field initialization in loops. Evaluate and check the selector before
crediting any arm's assignments.

This integrates switch into existing field-initialization and nullable-flow
analysis. It does not allow uninitialized enum, struct, inferred, or final
locals to be assigned later across arms. It adds no type-pattern smart casts
or new local initialization policy.

## Compiler representation and lowering

AST retains the selector, grouped labels, arm blocks, optional default, and
source ranges. HIR retains their checked nominal type and normalized constants,
with resolved constant symbols where applicable. Each arm body is stored and
analyzed once, even when several values select it.

Introduce a typed MIR switch terminator with one selector value, distinct
constant/target pairs, and an explicit default successor. Preserve enum type
identity through MIR verification; erase to its existing scalar tag only at
LLVM lowering. No instruction may follow the terminator in its block.

Implementation: enum terminators also retain an explicit invalid-tag successor
to an empty trap block. All CFG consumers enumerate it. A source default is
entered only after an unmatched tag passes the case-count guard. Phi-bearing
destinations use one LLVM edge bridge per unique MIR predecessor, including
shared case/default targets; other destinations require no edge bridge.

Verifier and CFG work must cover:

- valid selector definitions/types, exact label types, constant ranges, nominal
  enum ownership/tags, duplicate labels, and bounded record sizes;
- valid targets in the same body, complete successor/predecessor enumeration,
  reachability, and correct termination;
- grouped values sharing a destination without duplicate phi predecessor
  entries; phi inputs follow unique predecessor blocks, not case-edge count;
- constructor initialization dataflow across every successor; and
- selector value uses and every successor in GC liveness, including managed
  values and GC-bearing structs live across arm calls, joins, and loop transfers.

Emit LLVM's integer `switch` after verification, retaining multiway selection
so target lowering can choose branches or a table. Do not mandate jump tables
or emit a source-level linear comparison chain as the permanent representation.
LLVM requires distinct constant entries and a default destination; its exact
instruction contract is in the [LLVM language reference](https://llvm.org/docs/LangRef.html#switch-instruction).

For an integer without source default, the default successor is the join. For
an exhaustive enum without source default, unmatched bits go to an existing
trap mechanism, not a fabricated case or unchecked `unreachable`. An enum with
source default still guards the unmatched tag against the known enum case count
before entering that body. Valid but unlisted tags use the user fallback;
invalid tags trap. This introduces no user-visible enum/integer conversion or
new runtime service. Test corrupted internal values without adding an unsafe
source-language escape hatch.

Bound a switch to 65,536 value labels, counted before deduplication, and at most
65,537 arms including default. This admits the current maximum enum case set.
Use checked counts, heap-backed collections, and non-quadratic duplicate/coverage
checks; diagnose limits consistently in source and IR verification. Preserve
source order for diagnostics and deterministic normalized order for emitted
case tables. Large sparse integers must not allocate storage proportional to
the numeric span.

The 27.2 frontend checkpoint explicitly gated native/IR emission. Stage 27.3
removes that gate only with MIR, LLVM, GC, trap, and source-free verification.

## Packages, compatibility, and tooling

Existing artifacts already carry complete enum case sets and typed scalar
static values. Consumers must resolve labels and prove coverage from those
verified records without reopening dependency sources. Continue validating
visibility, canonical owner identity, and exact dependency digests.

MIR bodies are not serialized in the declaration view. No record shape,
physical calling convention, runtime layout, artifact-format or compiler/runtime
ABI revision,
process protocol, receipt schema, or manifest schema change is expected. Review
and explicitly approve any discovered compatibility change before implementing
it; do not silently extend the current format. Exact compiler identity still
invalidates prior cached builds.

Shuttle must mirror the three new reserved keywords in dependency-alias
validation; the compiler must reject them independently in explicit protocol
inputs. Package-name grammar does not change. Shuttle must not parse switch
bodies or interpret enum metadata. Its other work is shared verification of
source-free coverage, stale-dependency rejection, serial/parallel equivalence,
and failed-build output preservation.

Update VS Code keyword rules and switch snippets when syntax is implemented.
Check that expanded snippets compile and invalid variants are diagnosed. Keep
legacy generator redesign out of scope.

Implemented user guidance belongs in `documentation/reference/language/` and
`documentation/reference/types/enums.md`. Keep supported compilation modes
current at each checkpoint. Maintainer
grammar/flow/MIR/LLVM contracts and exit audits
remain in `docs/`; planning stays in this proposal, the roadmap, and TODO.

## Stages and verification gates

1. **27.1 — Contract:** review this draft, resolve objections, record explicit
   contract approval and implementation authorization. Draft completion alone
   does not close 27.1 or activate 27.2.
2. **27.2 — Frontend:** lexer/parser/AST, binding and constant validation,
   exhaustiveness, typed HIR/verifier, scoped transfers, return/constructor/
   nullable-flow analysis, and explicit native-lowering gating. Synchronize
   compiler/Shuttle keyword rules and editor highlighting.
3. **27.3 — Lowering:** MIR and verifier, CFG/phi/GC consumers, LLVM emission,
   invalid-tag traps, native and source-free compilation. Audit compatibility
   and retain existing versions unless a separate review requires a change.
4. **27.4 — Exit audit:** prove whole/separate native behavior, reordered and
   source-free dependencies, alias/keyword policy, serial/parallel artifacts,
   invalidation and output preservation. Finish public docs, maintainer
   contracts, snippets, and development/sanitizer/Rust/editor gates.

Required focused coverage includes signed minima/unsigned maxima and byte
selectors; grouped and duplicate labels; nominal mismatches and private
constants; every missing/default/no-match path; empty/malformed arms and parser
recovery; nested loop/switch transfers; exhaustive returns and final/required
field initialization with early exits; independent arm locals and non-null
facts; exactly-once selectors; malformed HIR/MIR; phi joins and forced GC across
arm calls; bounded large/dense/sparse selections; and enum/static-constant edits
in source-free dependencies. Retain regression coverage for pre-switch loops.

The stage closes only after all four substages and both repositories' shared
gates are complete. Deferred language features remain in the backlog.
