# Stage 47: Self-hosted definition parser

Status: **complete — 47.4 exit audit passed 2026-09-10**.

Stage 47 implements the definition half of Cloth's two-pass parser in the
self-hosted compiler at `F:\Cloth`. It consumes the immutable Stage 46
declaration result, parses only the recorded initializer and body intervals,
materializes the complete abstract syntax tree, seals its managed storage, and
publishes a verified tree.

See the [compiler roadmap](../../ROADMAP.md), [work ledger](../../TODO.md),
[Stage 45 parser foundations](stage_45_self_hosted_parser_foundations.md), and
[Stage 46 declaration parser](stage_46_self_hosted_declaration_parser.md).

## 1. Authority and scope

The C++23 `DefinitionPass` and `ExpressionParser` remain the observable-
behavior oracle throughout Stage 47. The self-hosted pass must match their
accepted expression and statement language, precedence, associativity, source
ranges, tree shape, validity, diagnostic categories, recovery, declaration
order, constant-parser limits, and deferred-region consumption.

Stage 47 is compiler-internal. It adds no Cloth source syntax and changes no
program accepted by production `clothc`. It performs no name resolution, type
checking, control-flow analysis, constant evaluation, HIR construction, or
lowering. Syntax nodes retain unresolved source structure only.

Completing Stage 47 does not transfer parser authority. The C++ parser remains
authoritative until a separately approved complete-parser parity and authority-
transfer audit covers declaration and definition behavior together.

## 2. Input and publication boundary

`DefinitionParser` receives:

- one verified immutable `DeclarationResult`;
- the result's retained `SourceFile` and EOF-terminated `TokenBuffer`;
- the file outline and ordered member outlines from that same result; and
- a compiler-driver-owned package constant budget when static fields are
  parsed as required constant initializers.

The parser validates that every interval it opens belongs to the supplied
declaration result. It cannot widen an interval, replace the source or token
buffer, rediscover a signature, inspect a neighboring member's tokens, or
change declaration order. A forged result or inconsistent interval is an
internal `StateError`, not source recovery.

One definition run owns one fresh `SyntaxStorage`. It emits exactly one
`DeclarationSyntax` for each member outline in source order, appends all
reachable expressions, statements, and blocks to that storage, seals the
storage exactly once, builds `FileSyntax` from the file outline, and publishes
it only through `VerifiedSyntaxTree.Build`.

`DefinitionResult` retains:

- the originating `DeclarationResult`;
- the published `VerifiedSyntaxTree`;
- all declaration and definition diagnostics in deterministic source order;
  and
- aggregate validity.

Declaration diagnostics are neither discarded nor duplicated. Lexical
diagnostics remain owned by `LexerResult`; a later compiler driver may present
the phase results together. A definition result is valid only when the
declaration result, every materialized declaration, every reachable syntax
node, and the combined diagnostic sequence are valid.

An abstract or interface function without source body tokens receives a
canonical valid empty block at the declaration-end point. That block represents
an absent executable body, not source `{}`. A malformed concrete function or
constructor with no usable body receives an invalid empty block at a point
inside its declaration range. These representations preserve the existing
non-null `FunctionSyntax` and `ConstructorSyntax` body handles.

## 3. Definition materialization

A field copies its declared type, name, visibility, `final`, and `static` state
from its outline. If an initializer interval is present, the definition pass
parses exactly one expression inside that interval. Any unconsumed token before
the interval limit produces a definition diagnostic and makes the field
invalid. An absent initializer remains absent.

A function copies its name, visibility, parameters, return type, throws types,
and modifiers from its outline. A concrete body interval produces one block;
an abstract or interface contract uses the canonical absent-body block. A
constructor also parses its optional base initializer interval before its body.

A constructor initializer has this grammar:

```text
constructor-initializer = type, "(", [ argument-list ], ")" ;
argument-list           = expression, { ",", expression } ;
```

The initializer retains its unresolved base type, ordered argument handles,
complete interval range, and validity. Base-constructor identity and legality
remain semantic work.

Invalid declaration outlines are still materialized when their retained
structure is safe to consume. Missing intervals produce invalid placeholders;
present verified intervals remain hard bounds. Definition errors invalidate
the owning declaration and file but do not suppress independently recoverable
later declarations.

Every arbitrary spelling remains source-backed through `SourceSpan`. Syntax
storage contains no semantic symbol, resolved type, HIR value, native pointer,
or reference into C++ memory.

## 4. Statement grammar

The accepted definition grammar is:

```text
block                = "{", { statement }, "}" ;

statement            = local-declaration
                     | return-statement
                     | if-statement
                     | while-statement
                     | for-statement
                     | switch-statement
                     | break-statement
                     | continue-statement
                     | block
                     | expression-statement ;

local-declaration    = [ "final" ], ( "var" | type ), identifier,
                       [ "=", expression ], ";" ;
return-statement     = "return", [ expression ], ";" ;
if-statement         = "if", "(", expression, ")", block,
                       [ "else", block ] ;
while-statement      = "while", "(", expression, ")", block ;
break-statement      = "break", ";" ;
continue-statement   = "continue", ";" ;
expression-statement = expression, ";" ;

for-statement        = "for", "(",
                       ( for-each-clause | traditional-for-clauses ),
                       ")", block ;
for-variable         = [ "final" ], ( "var" | type ), identifier ;
for-each-clause      = for-variable, "in", expression ;
traditional-for-clauses
                     = [ local-for-initializer | expression ], ";",
                       [ expression ], ";",
                       [ expression, { ",", expression } ] ;
local-for-initializer
                     = for-variable, [ "=", expression ] ;

switch-statement     = "switch", "(", expression, ")", "{",
                       switch-arm, { switch-arm }, "}" ;
switch-arm           = ( "case", expression, { ",", expression }
                       | "default" ), ":", block ;
```

Bodies are always braced. `else if` is not a distinct production; another
`if` may be nested inside an explicit `else` block. A traditional `for`
initializer contains at most one declaration or expression. Its update clause
may contain a comma-separated sequence. A `switch` requires at least one arm,
permits at most one `default`, and requires `default` to be last. Case-label
constancy, uniqueness, compatibility, and exhaustiveness remain semantic work.

`case` and `default` outside a direct switch-arm position are invalid
statements. `break` and `continue` are parsed structurally everywhere; loop and
switch placement is validated later.

An inferred local without an initializer, such as `var value;`, is retained as
syntactically well-formed structure and rejected by later type inference. The
syntax-tree verifier checks representation consistency; it cannot promote that
semantic rule into a definition-parser diagnostic. The same boundary applies to
a traditional `for` initializer.

## 5. Expression grammar and precedence

Expressions use the existing precedence levels, from lowest to highest:

| Level | Operators | Associativity |
| --- | --- | --- |
| 1 | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `<<=`, `>>=`, `&=`, `|=`, `^=` | right |
| 2 | `??` | right |
| 3 | `||` | left |
| 4 | `&&` | left |
| 5 | `|` | left |
| 6 | `^` | left |
| 7 | `&` | left |
| 8 | `==`, `!=` | left |
| 9 | `<`, `<=`, `>`, `>=`, `is`, `as` | left |
| 10 | `<<`, `>>` | left |
| 11 | `+`, `-` | left |
| 12 | `*`, `/`, `%` | left |

Assignment operators produce `AssignmentExpressionSyntax`; `??` produces
`NullCoalesceExpressionSyntax`; `is` and `as` consume a checked `TypeSyntax`;
all other table entries produce `BinaryExpressionSyntax`.

The remaining grammar is:

```text
unary-expression     = "throw", expression
                     | ( "++" | "--" | "!" | "+" | "-" | "~" ),
                       unary-expression
                     | postfix-expression ;

postfix-expression   = primary-expression, { postfix-suffix } ;
postfix-suffix       = "(", [ argument-list ], ")"
                     | "[", expression, "]"
                     | ".", identifier
                     | "?.", identifier
                     | "::", identifier
                     | "?", "::", identifier
                     | "!"
                     | "++"
                     | "--" ;

primary-expression   = identifier
                     | "super"
                     | literal
                     | "(", expression, ")"
                     | array-literal
                     | array-construction
                     | numeric-conversion
                     | integer-conversion ;

literal              = integer-literal | float-literal | string-literal
                     | character-literal | "true" | "false" | "null" ;
array-literal        = "[", [ expression, { ",", expression } ], "]" ;
array-construction   = type, "[", ":", expression, "]" ;
numeric-conversion   = numeric-type, "(", expression, ")" ;
integer-conversion   = primitive-type, "::", identifier,
                       "(", expression, ")" ;
```

Postfix call, index, declared member access, safe member access, meta access,
safe meta access, null assertion, and update operations produce their existing
dedicated syntax kinds. A postfix update terminates the postfix chain. The
parser records an integer meta-conversion operation as source spelling;
semantic analysis restricts it to `wrap` or `sat` and an integer target.
Primitive `::parse` remains an ordinary meta-call syntax shape rather than an
integer conversion.

Array element typing, assignment targets, update targets, callable identity,
member/meta availability, nullability, throw effects, conversion legality, and
operator types remain semantic work.

## 6. Bounded parsing and constant contexts

Every expression parser instance owns a current token index and an exclusive
limit inherited from its field, constructor-initializer, or body interval.
Lookahead saturates at that limit. A child parser may narrow but never widen the
interval. Successful parsing of an initializer or list element must either
reach its grammatical delimiter or report a diagnostic and make progress.

Ordinary runtime expression and block nesting cannot rely on native call-stack
depth proportional to source. The self-hosted implementation uses explicit
managed parse frames or an equivalent bounded iterative algorithm. It adds no
new source-visible nesting limit relative to the C++ oracle.

Static field initializers retain the established package-wide required-
constant parser budgets:

- 65,536 static constant declarations per owning package;
- 65,536 expression nodes per constant initializer;
- 1,048,576 expression nodes across the package's static initializers;
- expression nesting depth 256, counting the root; and
- 4,096 source bytes per numeric literal spelling.

The package driver owns and deterministically shares that budget across source
files. Dependency packages do not consume the current package's budget. A
limit emits one bounded structured diagnostic, consumes no tokens beyond the
current initializer, and cannot publish a partially valid constant tree.

Switch parsing retains 65,536 total value labels and 65,537 total arms,
including `default`. All additions, counts, source offsets, token indices,
syntax ordinals, and allocation sizes are checked before mutation.

## 7. Diagnostics and recovery

Stage 47 extends `ParseDiagnosticKind` with definition-specific structured
kinds. Distinct kinds cover expected expressions and checked types; missing
group, call, index, array, conversion, and initializer delimiters; missing
member/meta names; unexpected trailing initializer tokens; block and statement
punctuation; malformed local and `for` clauses; misplaced switch labels;
malformed or repeated switch arms; switch limits; and constant-parser limits.

Parser decisions use token kinds and state, never rendered English. A driver or
test adapter may render a diagnostic kind after parsing. Every diagnostic
retains its primary source range and optional related range and freezes in
stable primary-source order after declaration and definition diagnostics are
merged.

Recovery boundaries are:

- an expression stops at its supplied delimiter or interval limit;
- calls, array literals, constructor arguments, and switch labels synchronize
  at a comma or their closing delimiter;
- a statement synchronizes at its semicolon, enclosing right brace, or a
  credible next statement starter;
- a switch arm synchronizes at a top-level `case`, `default`, or the switch's
  right brace while respecting nested braces; and
- a block cannot consume the right brace or token interval owned by its parent.

Every recovery loop must consume a token, move to a strictly later
synchronization point, or terminate. Invalid expression and statement nodes
retain recoverable source structure, and a malformed statement cannot suppress
a later independent statement, block, member, or file.

## 8. Complexity, storage, and GC ownership

Let `R` be the total number of tokens across all recorded deferred intervals
and `N` the number of published syntax nodes. Definition parsing is `O(R + N)`
time and `O(N)` retained memory. No initializer, body, expression, or recovery
region is reparsed. The pass cannot rescan declaration signatures or the full
file to locate work already recorded by Stage 46.

Unknown-length child lists use specialized 64-slot segmented builders and
freeze once into exact immutable sequences. They leave fewer than one
construction page unused per active builder and do not introduce a general
collection API. Storage page placement and handle ordinals are implementation
details, not syntax identity or parity data.

The published tree strongly retains its source-backed spans and one sealed
syntax storage. Handles do not keep builders, cursors, parse frames, diagnostic
collectors, obsolete pages, or partially built alternative trees alive.
Focused GC tests must retain published trees across allocation pressure and
prove abandoned construction state is collectible.

Whole-tree verification must handle source-proportional nesting without native
stack exhaustion. It validates every reachable handle once, rejects foreign
handles or invalid ownership, and preserves the Stage 45 range, kind/payload,
child-validity, and source-identity invariants.

## 9. Source organization

Stage 47 adds production files only with implemented responsibilities:

```text
frontend/parser/
  declaration/                    outline pass and retained intervals
  definition/                     materialization and tree publication
  expression/                     precedence and expression work state
  statement/                      blocks, statements, loops, and switches
  support/                        cursors, facts, ranges, types, and budgets
  storage/                        typed child-list construction
tests/self_host/
  src/checks/                     lexer, parser, and syntax checks
  src/fixtures/                   focused construction fixtures
  src/parity/                     canonical differential adapters
  testdata/                       deterministic lexer and parser inputs
```

Files may be combined only when they retain one cohesive responsibility; they
must be split when state or recovery rules become independently owned. Parser
production code cannot import the self-host test package. Syntax nodes and
storage cannot import parser orchestration. Canonical record writers and
executable checks are test adapters, not compiler CLI contracts.

## 10. Checkpoint plan

1. **47.1 — Contract (complete).** Freeze input and publication boundaries,
   definition and expression grammar, exact precedence, recovery, resource
   limits, explicit-depth handling, storage and GC ownership, organization,
   parity, compatibility, and non-goals.
2. **47.2 — Expression parser (complete).** Implement bounded expression
   parsing for every existing syntax kind, exact precedence and associativity,
   postfix chains, constructor and field initializer expressions, constant-
   parser budgets, structured diagnostics, recovery, verification, and focused
   GC tests.
3. **47.3 — Definitions and statements (complete).** Materialize every
   outline in source order; implement blocks, locals, returns, conditionals,
   both `for` forms,
   `while`, `switch`, loop control, nested blocks, final tree publication, and
   canonical C++/Cloth definition-record differential coverage.
4. **47.3.1 — Compiler/test separation (complete).** Move checks, probes,
   fixtures, and parity adapters out of the production package; establish the
   `clothc` driver boundary; organize parser sources by owned grammar domain;
   and verify the resulting package-artifact boundary.
5. **47.4 — Integration and exit audit (complete).** Close complete definition
   parity,
   real-bootstrap and malformed/adversarial coverage, precedence and recovery,
   resource and complexity bounds, GC, native and both-target builds,
   serial/parallel determinism, Shuttle reuse and failure preservation,
   sanitizers, documentation, and repository gates.

The exit audit compares 43 focused valid and malformed definition inputs and
all 177 production compiler sources against the C++ oracle. Direct, repeated
serial, and parallel test builds agree on every record. Exact parser resource
boundaries, 4,096-level ordinary nesting, retained-tree GC checks, native and
wasm32 builds, Shuttle warm reuse and failure preservation, and development and
ASan/UBSan matrices pass. The audit changes no source grammar or compatibility
version.

No checkpoint may silently expand the accepted source grammar. A grammar or
language change requires its own approved contract before implementation.

## 11. Compatibility and verification

Stage 47 retains artifact/compiler/runtime compatibility **8/7/11**,
process/receipt/manifest/toolchain schemas **2/1/1/1**, and compiler-paired
`cloth` **v0.5.0**. The standard library, runtime, editor, public compiler CLI,
and Shuttle require no production behavior change.

Canonical definition records assign stable traversal-local IDs rather than
exposing syntax owner identities, page numbers, slots, addresses, or allocation
order. Records include declarations, constructor initializers, blocks,
statements, expressions, types, source spans and ranges, child order, observable
or structurally derived validity, and structured diagnostics. A per-node
validity flag may be normalized only when the C++ oracle stores no corresponding
state; normalization cannot hide a tree-shape, recovery, diagnostic, owning-
declaration, block-validity, or aggregate-validity difference.

Differential coverage includes every operator and precedence boundary; nested
postfix chains; all literal, conversion, array, nullable, throw, and type-
relation forms; every statement kind; both `for` forms; switches with multiple
labels and `default`; abstract and concrete functions; field and constructor
initializers; malformed delimiters and list elements; statement and arm
recovery; exact resource boundaries; and stable invalid nodes.

47.4 compares canonical full-tree records for every production `.co` file in
the real bootstrap. Direct, serial Shuttle, warm-reuse, and parallel outputs
must agree. Native execution, x86-64 and wasm32 checking, development and
sanitizer matrices, GC lifetime, failure-output preservation, formatting,
documentation links, and repository whitespace checks must pass.

## 12. Non-goals

Stage 47 does not add or change source syntax; resolve imports, names, members,
types, overloads, operators, effects, nullability, constants, or control flow;
construct symbols, HIR, MIR, ABI, LLVM IR, artifacts, or executables; execute
compile-time code; replace the C++ parser; expose a compiler library API; add
generics, traits, lambdas, pattern matching, ranges, `else if` syntax,
unbraced bodies, multidimensional arrays, safe indexing, interpolation, or new
operators; create a lossless or incremental tree; introduce general
collections; or change any compatibility version.
