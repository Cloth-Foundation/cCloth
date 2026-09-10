# Stage 46: Self-hosted declaration parser

Status: **complete — 46.4 exit audit passed 2026-09-09**.

Stage 46 implements the declaration half of Cloth's two-pass parser in the
self-hosted compiler at `F:\Cloth`. It consumes the completed Stage 44 token
buffer and produces the immutable declaration result required by the future
definition pass. It does not parse field initializers, constructor initializers,
statements, or expressions.

See the [compiler roadmap](../../ROADMAP.md), [work ledger](../../TODO.md), and
[Stage 45 parser-foundation contract](stage_45_self_hosted_parser_foundations.md).

## 1. Authority and scope

The C++23 declaration pass remains the observable-behavior oracle throughout
Stage 46. The self-hosted pass must match its accepted declaration language,
source ranges, declaration order, validity, diagnostic categories, recovery,
and deferred token boundaries. A parity adapter may normalize implementation-
specific storage, but it cannot hide a semantic or recovery difference.

Stage 46 is compiler-internal. It adds no Cloth source syntax and changes no
user program accepted by `clothc`. The new types remain ordinary contents of
the bootstrap compiler package and do not enter package artifacts, the runtime
ABI, the standard-library API, or the Shuttle protocol.

The C++ parser remains authoritative after 46.4. Authority can move only after
the separately approved definition pass and complete parser parity audit.

## 2. Input and publication boundary

The declaration parser receives:

- one immutable `SourceFile`;
- its immutable, EOF-terminated `TokenBuffer`; and
- package identity supplied by the compiler driver rather than inferred by
  filesystem discovery.

The buffer must contain at least one token, contain exactly one `kEof` token at
the end, and contain tokens whose source ownership, spans, ranges, and offsets
are monotonic and valid for the supplied file. A malformed buffer is an internal
`StateError`; it is not user source recovery.

Successful publication produces one immutable `DeclarationResult` retaining:

- the source and token buffer that back all spans and token intervals;
- the implicit file name, package name, qualified name, and capitalization-
  derived visibility;
- file-type kind, explicit-envelope state, and type modifiers;
- ordered imports, base type, interfaces, and enum cases;
- one ordered `MemberOutlineSequence` in source order;
- ordered structured declaration diagnostics; and
- aggregate validity.

Publication verifies the complete result before exposing it. Builders, cursors,
measurement state, and recovery state are not reachable from the published
result. A result may be invalid because of recovered source errors while still
containing every independently recoverable later declaration.

Stage 46 does not create `FileSyntax`, `FunctionSyntax`, `ConstructorSyntax`, or
`FieldSyntax`. Those complete nodes require definition-pass expression and block
handles. A later definition-pass stage will combine this declaration result
with one sealed `SyntaxStorage` and publish `VerifiedSyntaxTree`.

## 3. Declaration data

`TokenIndexRange` is a half-open interval `[begin, end)` into the retained token
buffer. Both indices use checked `int32` values and satisfy
`0 <= begin <= end <= eofIndex`. It never includes `kEof`.

Each member outline retains:

- field, function, or constructor kind;
- source-backed name and capitalization-derived visibility;
- unresolved declared type or return type when present;
- ordered parameters, including each parameter's `final` state;
- ordered unresolved `throws` types and whether `throws` was written;
- applicable `static`, `final`, `abstract`, and `override` flags;
- its complete source range and beginning token index;
- an optional field-initializer token range;
- an optional constructor-initializer token range;
- an optional callable-body token range; and
- declaration validity.

A field initializer range excludes `=` and `;`. A constructor-initializer range
excludes `:` and the body. A body range includes its opening and closing braces.
An unterminated body ends immediately before EOF and makes its outline invalid.
An absent range is distinct from a present empty range; accepted initializers
cannot be empty.

Imports, types, parameters, enum cases, and outlines use exact immutable
sequences. Arbitrary source spelling remains in `SourceSpan`; the parser does
not copy lexemes into unrelated strings. The result contains no semantic symbol
IDs, resolved type IDs, native pointers, or references into C++ storage.

## 4. Accepted grammar

The grammar below describes accepted declaration input. Recovery may retain an
invalid record for malformed input but does not make that spelling valid.

```text
file                 = { import-declaration },
                       ( explicit-file-type | { member-declaration } ), EOF ;

import-declaration   = "import",
                       ( type-import [ "as" identifier ] | wildcard-import ),
                       ";" ;
type-import          = identifier | package-path, "::", identifier ;
wildcard-import      = package-path, ".", "*" ;
package-path         = identifier, { ".", identifier } ;

explicit-file-type   = { file-modifier }, file-type, file-type-clauses,
                       "{", file-type-contents, "}" ;
file-modifier        = "abstract" | "sealed" ;
file-type            = "class" | "interface" | "enum" | "struct" | "error" ;
file-type-clauses    = [ ":", type-name-list ],
                       [ "is", type-name-list ] ;
type-name-list       = type-name, { ",", type-name } ;
file-type-contents   = enum-cases | { member-declaration } ;
enum-cases           = enum-case, { ",", enum-case }, [ "," ] ;
enum-case            = identifier ;

member-declaration   = field-declaration
                     | function-declaration
                     | constructor-declaration ;

field-declaration    = [ "static" ], [ "final" ], type, identifier,
                       [ "=", deferred-field-initializer ], ";" ;

function-declaration = { function-modifier }, "func", identifier,
                       parameter-list, [ ":", type ], throws-clause,
                       ( deferred-body | ";" ) ;
function-modifier    = "abstract" | "override" | "static" | "final" ;

constructor-declaration = identifier, parameter-list, throws-clause,
                          [ ":", deferred-constructor-initializer ],
                          deferred-body ;

parameter-list       = "(", [ parameter, { ",", parameter } ], ")" ;
parameter            = [ "final" ], type, identifier ;
throws-clause        = [ "throws", type, { ",", type } ] ;

type                 = type-name, [ "?" ], [ "[", "]", [ "?" ] ] ;
type-name            = identifier | primitive-type ;
primitive-type       = "int" | "int8" | "int16" | "int32" | "int64"
                     | "uint" | "uint8" | "uint16" | "uint32" | "uint64"
                     | "float" | "float32" | "float64"
                     | "bool" | "char" | "byte" | "void" | "object" ;
```

`T?[]`, `T[]?`, and `T?[]?` preserve nullable-element and nullable-array state
independently. Repeated nullable qualification and more than one array suffix
are diagnosed. `void` remains syntactically a type; contextual rejection belongs
to later semantic analysis.

An unwrapped file is an implicit class whose name is its file stem. An explicit
file-type envelope never repeats that name. Imports precede the envelope or the
first member. No token may follow a closed explicit envelope.

An interface may list parent interfaces after `:`. A class, struct, or error may
name one base after `:` and interfaces after `is`. Enums accept neither clause.
Interface contents contain only unmodified function contracts terminated by
`;`. Interface constructors, fields, modified contracts, and function bodies
are diagnosed.

Enums contain at least one case. Case names are unique and public regardless of
capitalization. The existing 65,536-case limit remains exact.

Function modifiers may appear in any order but cannot repeat. Abstract and
interface functions use `;`; concrete functions require a body. Field modifier
order remains `static final`. Constructors accept no modifier and use the file
stem, its first-letter-lowercase form, or its underscore-prefixed form. Their
visibility continues to follow the spelling actually used.

Nested type keywords remain reserved and unsupported. The declaration parser
diagnoses and skips their declaration without treating them as accepted nested
types.

## 5. Deferred regions and the two-pass guarantee

The declaration pass parses signatures only. It may balance delimiters to find
the end of a deferred region, but it cannot invoke expression or statement
grammar, append expression or block nodes, or interpret names inside that
region.

Body discovery balances braces and includes the matched pair. Field initializer
discovery balances parentheses and brackets before recognizing its terminating
semicolon or a credible following member boundary. Constructor-initializer
discovery balances parentheses and stops at the body-opening brace. Literal
contents cannot affect delimiter depth because the lexer already emitted each
literal as one token.

Every deferred interval is traversed once. Storage measurement, verification,
duplicate checking, diagnostic ordering, and record emission are not additional
grammatical passes and cannot reparse a deferred expression or statement.

The future definition pass receives only an outline's recorded interval for an
initializer or body. It cannot scan beyond that interval, rediscover signatures,
or reorder members.

## 6. Cursor, complexity, and resource limits

`TokenCursor` owns a current index and an exclusive limit. Reads and lookahead
saturate at the final EOF token, advancement never passes the limit, and a
subcursor cannot widen the interval supplied by its caller. Parser routines do
not retain a mutable cursor after publication.

Each grammar or recovery loop must consume a token, advance to a strictly later
synchronization point, or terminate. This invariant is checked in focused tests
so malformed input cannot hang the compiler.

Parsing and delimiter discovery are linear in token count. Duplicate validation
may use deterministic sorting and is bounded by `O(D log D)` for `D`
declarations; it cannot use an unbounded quadratic scan or a collision-sensitive
public hash. Total retained memory is `O(T)` for `T` tokens and outlines.

Specialized segmented builders retain imports, enum cases, parameters,
outlines, and diagnostics while counts are unknown. They freeze once into exact
arrays and leave fewer than one construction page unused per builder. They do
not create a general collection API or allocate source-sized nullable arrays.
Every count, addition, and capacity conversion is checked before allocation.

The existing 64 MiB source limit, `int32` token indices, 65,536 enum-case limit,
and terminal allocation-failure policy remain unchanged. Limit failures produce
one bounded diagnostic where they represent source scale; broken internal count
or storage invariants throw `StateError`.

## 7. Duplicate and visibility rules

File, field, function, constructor, and parameter visibility continues to be
derived from the first character of its declared name: uppercase is public;
lowercase or underscore-leading is private. Visibility is recorded, not resolved
against an importing package.

Within one file:

- a field conflicts with any previous member of the same name;
- a function conflicts with a non-function of the same name;
- functions with the same name may overload only when their parameter type
  signatures differ;
- all accepted constructor spellings share one overload set; and
- constructors may overload only when their parameter type signatures differ.

Nullable annotations do not create distinct overload signatures. The
declaration pass compares unresolved type spelling and array shape without
resolving aliases or nominal identities. Any collision requiring resolution
remains semantic work.

Duplicate enum cases and member signatures retain the later declaration as an
invalid record and attach the previous declaration range as structured related
context. They do not discard either source occurrence.

## 8. Diagnostics and recovery

`ParseDiagnostic` contains a declaration-specific kind, primary `SourceRange`,
and optional related range. English rendering is a driver responsibility;
parser logic does not match or construct prose to make decisions. Diagnostics
freeze in deterministic primary-source order, with stable encounter order as a
tie-breaker.

Source errors do not throw. Recovery boundaries are:

- imports synchronize at their semicolon or EOF;
- parameters synchronize at a comma, closing parenthesis, body brace, or the
  supplied declaration limit;
- fields synchronize at a semicolon, explicit-envelope closing brace, credible
  member start, or EOF;
- functions and constructors use their balanced deferred-body boundary;
- enum cases synchronize at a top-level comma or the envelope's closing brace;
  and
- top-level recovery stops at a credible member start, envelope boundary, or
  EOF.

Missing punctuation, invalid modifier placement, malformed types and import
paths, empty or duplicate enum cases, invalid constructor names, conflicting
members, unsupported nested types, unterminated bodies, misplaced imports, and
tokens after an explicit envelope all receive distinct structured kinds.

Recovery cannot consume the closing brace owned by an outer explicit envelope
unless it is completing that envelope. A recovered declaration, enclosing
explicit type, and result become invalid; later independent declarations remain
available. Lexical diagnostics remain owned by `LexerResult` and are not copied
into the declaration diagnostic sequence.

## 9. Source organization

Stage 46 adds production files only when their responsibility is implemented:

```text
frontend/parser/
  TokenBufferVerifier.co         complete source and coordinate ownership
  TokenCursor.co                 bounded token navigation
  TokenIndexRange.co             checked half-open token interval
  DeclarationOutline.co          immutable member signature and intervals
  FileDeclarationOutline.co      immutable file identity and envelope state
  MemberOutlineBuilder.co        segmented source-order construction
  MemberOutlineSequence.co       exact ordered publication
  DeclarationResult.co           verified declaration-pass result
  DeclarationResultVerifier.co   complete publication invariants
  DeclarationParser.co           file-level declaration orchestration
  DeclarationParseState.co       bounded mutable parse construction state
  ImportDeclarationParser.co     import grammar and recovery
  FileTypeDeclarationParser.co   explicit file kinds and type clauses
  EnumDeclarationParser.co       enum-case grammar and recovery
  TypeParser.co                  unresolved type shapes
  ParameterParser.co             callable parameter lists
  ThrowsClauseParser.co          declared error-effect lists
  FieldDeclarationParser.co      field signatures and initializer ranges
  FunctionDeclarationParser.co   function signatures and body ranges
  ConstructorDeclarationParser.co constructor signatures and ranges
  DeferredRegionLocator.co       balanced two-pass interval discovery
  EnumCaseDuplicateValidator.co  deterministic enum collision validation
  MemberDuplicateValidator.co    deterministic member collision validation
  storage/                       typed 64-slot pages and builders

frontend/diagnostic/
  ParseDiagnosticKind.co         structured declaration diagnostic identity
  ParseDiagnostic.co             primary and related ranges
  ParseDiagnosticBuilder.co      segmented source-order construction
  ParseDiagnosticPage.co         bounded construction page
  ParseDiagnosticSequence.co     immutable ordered publication
```

Additional focused files are justified only by a complete responsibility such
as import parsing, type parsing, or deterministic outline validation. Production
parser code cannot import `bootstrap`; syntax and token packages cannot import
parser orchestration. `DeclarationParser.co` and grammar-domain components enter
this directory only with 46.3.

Temporary declaration record writers and executable checks live under
`bootstrap/`. Test adapters are not public compiler APIs.

## 10. Checkpoint plan

1. **46.1 — Contract (complete).** Freeze the declaration grammar, immutable
   result and outline model, cursor and range rules, two-pass boundary,
   diagnostics, recovery, complexity, organization, compatibility, parity, and
   non-goals.
2. **46.2 — Parser substrate (complete).** Implement verified token intervals
   and cursor, structured declaration diagnostics, segmented outline
   construction, exact immutable sequences, result verification, and focused
   malformed-state, boundary, progress, and GC tests without declaration
   grammar.
3. **46.3 — Declaration grammar (complete).** Implement imports, implicit and
   explicit file types, inheritance and conformance clauses, enum cases, types,
   fields, function signatures, constructors, modifiers, throws clauses,
   deferred regions, duplicates, and deterministic recovery. Add the canonical
   C++ and Cloth declaration-record adapters and differential corpus.
4. **46.4 — Integration and exit audit (complete).** Close complete declaration parity,
   real-bootstrap coverage, malformed and adversarial inputs, complexity and
   resource limits, GC lifetime, determinism, native and both-target builds,
   Shuttle, sanitizers, documentation, and repository gates.

A separately approved definition-parser stage begins only after 46.4 passes.

## 11. Compatibility and verification

Stage 46 retains artifact/compiler/runtime compatibility **8/7/11**,
process/receipt/manifest/toolchain schemas **2/1/1/1**, and compiler-paired
`cloth` **v0.5.0**. Shuttle continues to treat bootstrap sources as opaque
compiler inputs. The standard library and editor require no production change.

46.2 verifies empty and one-token intervals, EOF saturation, exact bounds,
foreign-source rejection, segmented page boundaries, freeze-once behavior,
invalid publication, progress sentinels, and collection of obsolete builders.

46.3 differential coverage includes every accepted grammar production and each
recovery boundary, implicit and explicit file types, all file kinds, modifier
orders and duplicates, nullable array shapes, empty and large lists, overloaded
and conflicting members, constructor spellings, balanced and unterminated
deferred regions, and stable diagnostics.

46.4 parses every `.co` file in the real `F:\Cloth` bootstrap and compares
canonical declaration records with the C++ oracle. Direct, serial Shuttle,
warm-reuse, and parallel builds must agree. Native execution, x86-64 and wasm32
checking, development and sanitizer matrices, failure-output preservation,
formatting, documentation links, and repository whitespace checks must pass.

The completed audit compares 28 fixed declaration inputs, four generated scale
and resource-bound inputs, and all 185 production bootstrap sources. It crosses
the exact 65,536-case enum boundary, 4,096 declarations, and 8,192 nested
delimiters while retaining bounded diagnostics and deterministic records. All
355 development and 355 ASan/UBSan CTests pass, including direct, serial,
parallel, warm-reuse, native, both-target, GC, and failed-output checks. Shuttle,
editor, formatting, documentation-link, self-hosted style, and repository gates
also pass.

## 12. Non-goals

Stage 46 does not parse field or constructor initializers, callable bodies,
statements, or expressions; publish a complete syntax tree; perform name or type
resolution; create semantic symbols; lower HIR, MIR, ABI, or LLVM IR; replace the
C++ parser; expose a compiler library API; add nested types, generics, attributes,
macros, annotations, or new import forms; create a lossless or incremental tree;
add general collections; change diagnostics shown by the production `clothc`
CLI; or change any compatibility version.
