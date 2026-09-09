# Stage 45: Self-hosted parser storage and syntax tree foundations

Status: **complete — 45.4 exit audit completed 2026-09-09**.

Stage 45 establishes the storage and tree model needed by the self-hosted Cloth
parser at `F:\Cloth`. It does not add source syntax or parse the complete
language. The C++23 parser remains the temporary behavior oracle while the
replacement advances through separately approved stages.

See the [compiler roadmap](../../ROADMAP.md) and [work ledger](../../TODO.md).

## 1. Bootstrap boundary

New compiler implementation belongs in Cloth. Production changes to `cCloth`
must repair correctness or security, preserve supported platform and LLVM
compatibility, add test-only parity adapters, or unlock a documented
self-hosting requirement. Ordinary language features do not enter the C++
bootstrap after this checkpoint.

The C++ parser remains authoritative for observable syntax behavior until a
later parser parity audit passes. Observable behavior includes declaration and
member order, tree shape, source ranges, validity, diagnostics, and recovery.
Its C++ variants, vectors, allocation strategy, and concrete class layout are
not language contracts and need not be reproduced in Cloth.

Stage 45 is compiler-internal. These types are ordinary contents of the
bootstrap compiler package only; they do not become metadata in user package
artifacts or enter the runtime ABI, Shuttle protocol, or standard-library API.

## 2. Tree model

The parser produces an abstract syntax tree rather than a lossless concrete
syntax tree. Whitespace, comments, separators, and delimiters remain available
through the immutable token buffer and source bytes but do not become nodes.

The root represents the file's implicit type and retains its source identity,
file-type kind, visibility, modifiers, inheritance, interfaces, imports, enum
cases, declarations, and original member order. Declaration families cover
fields, functions, and constructors. Body syntax is partitioned into blocks,
statements, and expressions. Type syntax remains unresolved and source-backed;
name and type resolution belong to semantic analysis.

Each stored node has:

- one exact syntax kind from its node family;
- one half-open `SourceRange` belonging to the parsed `SourceFile`;
- ordered child handles or an ordered immutable child sequence; and
- explicit validity when recovery produced a partial construct.

Invalid expressions and statements are real nodes. A missing optional child is
represented separately and never doubles as an invalid node. Names, literal
spellings, and other source text use `SourceSpan`; syntax storage does not copy
arbitrary lexemes into new strings.

The tree has no parent links. Children point downward, declaration and child
order follow source order, and a published tree contains no dangling or cross-
result handles.

## 3. Two-pass parsing

Cloth parsing has exactly two grammatical passes over each token buffer.

The declaration pass reads imports, the file-type envelope, modifiers,
inheritance and conformance clauses, enum cases, and member signatures. It
records member outlines and half-open token-index ranges for field
initializers, constructor initializers, and callable bodies. It may balance
delimiters to locate those ranges but does not parse their expressions or
statements.

The definition pass consumes the immutable declaration result. It parses the
recorded initializers and bodies, creates blocks, statements, and expressions,
and completes declarations without rediscovering or reordering signatures.
Malformed declarations may omit a body range; the definition pass never scans
outside an outline supplied by the first pass.

Both passes operate on the same immutable EOF-terminated `TokenBuffer` and
`SourceFile`. A project may schedule declaration passes for all files before
definition passes, but scheduling cannot change a file's result. Import
resolution, cross-file symbols, overload selection, and types remain semantic
work rather than a hidden parser pass.

Storage sizing does not introduce another grammar pass. Neither pass reparses
a construct merely to count nodes or children.

## 4. Storage and identity

Expressions, statements, and blocks use distinct typed handles. Handles are
created monotonically by one owning `SyntaxStorage`, are stable for that parse
result's lifetime, and resolve in constant time. Their deterministic ordinal is
useful for verification but is not a serialized format or public ABI. A handle
from another storage is an internal invariant violation, not a source error.

Storage is append-only while parsing and read-only after publication. It uses
fixed-capacity segmented pages with checked append operations. A handle names
its owning page and slot directly; lookup does not walk earlier pages. The last
page may be partially occupied, so storage overhead is bounded by fewer than
one arena page instead of a source-sized unused array.

Ordered variable-length children use specialized segmented builders. A builder
tracks a checked `int32` count, preserves insertion order, and freezes once into
an exact-sized immutable sequence. It must not repeatedly copy an existing
prefix, manufacture a general collection API, or expose nullable empty slots to
tree consumers.

Resolution checks family, owner, slot, and occupancy at the storage boundary;
mutation and publication operations check the current storage state. Parser
loops must consume a token, move to a later synchronization point, or
terminate. Counts and token indices remain `int32`; the existing 64 MiB source
limit keeps valid source-derived counts representable. Allocation failure
retains Cloth's existing terminal runtime policy.

## 5. Ownership and GC lifetime

`ParseResult` owns the source, token buffer, declaration result, syntax storage,
root tree, and ordered parse diagnostics needed by later phases. Nodes and
pages use ordinary managed Cloth references. There are no native pointers,
manual release operations, global node registries, or ownership transferred to
the standard library.

Parser cursors and temporary child builders become unreachable when parsing
finishes. The tree remains reachable only through `ParseResult` and consumers
that deliberately retain it. Semantic lowering must not make syntax globally
immortal; after downstream compiler state releases the result, tracing garbage
collection may reclaim the complete graph, including cycles internal to arena
handles.

The representation therefore works with the current managed runtime and does
not require a future storage redesign when garbage-collection policy evolves.

## 6. Recovery and diagnostics

Ordinary syntax errors produce structured parse diagnostics and a bounded
partial tree; they do not throw a Cloth error or surface a storage `StateError`
to the user. Diagnostics remain in deterministic source order. Complete parse
diagnostic kinds and message parity belong to the stages that implement the
declaration and definition passes.

Recovery follows the C++ oracle's grammatical boundaries:

- top-level recovery stops at the next credible member or file boundary;
- statement recovery stops at a semicolon, closing brace, or statement start;
- expression recovery creates an invalid expression and leaves the caller at a
  documented delimiter; and
- every recovery path makes progress or terminates at its supplied token limit.

A declaration, block, or root is invalid when it contains a directly recovered
syntax error. Independent later constructs remain representable so one failure
does not erase the rest of the file. Internal failures cover malformed token
buffers, invalid handles, storage overflow, and invalid seal/publication state;
they are distinct from source diagnostics.

## 7. Source organization

Stage 45 establishes these dependency boundaries in the bootstrap repository:

```text
frontend/parser       two-pass orchestration and grammar ownership
       |       \
       |        -> frontend/diagnostic
       -> frontend/syntax
                    -> frontend/token
                    -> frontend/source
       -> frontend/token
              -> frontend/source
```

`frontend/syntax` owns node families, typed handles, segmented storage, and
immutable child sequences. It may depend on source and token data but cannot
import parser orchestration or semantic analysis. `frontend/parser` consumes
tokens and creates syntax. Semantic stages may consume syntax later but syntax
never imports them.

Files are introduced only by the checkpoint that implements their complete
responsibility. The existing `STYLE.md` and `ARCHITECTURE.md` remain binding;
Stage 45 does not permit placeholder directories, catch-all helpers, or one
file containing the whole tree.

## 8. Checkpoint plan

1. **45.1 — Contract (complete).** Freeze the bootstrap boundary, abstract tree,
   two grammatical passes, storage and typed-handle invariants, GC lifetime,
   recovery, organization, compatibility, verification, and non-goals.
2. **45.2 — Syntax storage (complete).** Implement typed handles, append-only
   segmented arenas, child builders and immutable sequences, sealing, checked
   access, and focused boundary tests without parser grammar.
3. **45.3 — Syntax tree (complete).** Implement the source root, unresolved
   type syntax, imports, declarations, blocks, statements, expressions, invalid
   nodes, and a deterministic bootstrap tree-record adapter.
4. **45.4 — Exit audit (complete).** Close storage, tree-shape, source-range,
   malformed-state, GC, determinism, native, both-target, Shuttle, sanitizer,
   documentation, and repository gates.

Declaration grammar begins only after Stage 45 exits. Definition parsing and
full differential parser parity also require later approved stages.

### 45.2 implementation record

The bootstrap now stores expressions, statements, and blocks in one managed,
heterogeneous arena. Fixed-capacity 64-node pages provide append-only growth;
each handle retains its owner, page, slot, and monotonic arena ordinal for
constant-time checked resolution. `ExpressionId`, `StatementId`, and `BlockId`
are separate managed types over one private handle payload and cannot be
substituted at typed storage entry points.

Specialized expression and statement builders use 64-handle construction
pages, reject mixed owners, freeze once, and publish exact-sized ordered
sequences. The published sequence owns its copied backing array and provides
only checked indexed reads. Block lists are not generalized prematurely;
Stage 45.3 will introduce only the child families required by the tree model.

The node-family interfaces establish storage boundaries without choosing
concrete syntax kinds before 45.3. Bootstrap checks cover empty storage, both
sides of the 64-node and 64-child boundaries, mixed families, foreign owners,
stable ordinals, exact order, empty sequences, reads after sealing, and four
preserved runtime failures: sealed append, foreign read, mixed-owner children,
and child bounds. The cross-repository native, wasm32, direct, Shuttle,
determinism, warm-reuse, and lexer-parity audit passes with these checks.

### 45.3 implementation record

The bootstrap syntax package now owns concrete, grammar-neutral nodes under
`tree/`, `declaration/`, `expression/`, and `statement/`. `FileSyntax` retains
the source, implicit file-type identity, visibility, imports, inheritance,
interfaces, enum cases, sealed storage, and one immutable declaration sequence.
That sequence records fields, functions, and constructors directly in source
order instead of reproducing the C++ oracle's parallel vectors and member-index
adapter.

Unresolved types and arbitrary source spellings use `SourceSpan`; variable-
length imports, enum cases, types, parameters, declarations, and switch arms
publish through exact immutable sequences. The arena contains concrete block
nodes, explicit invalid expression and statement nodes, all 24 expression
forms, and all 12 statement forms currently represented by the C++ oracle.
Nullable handles represent omitted optional children and remain distinct from
invalid nodes.

Bootstrap checks construct every kind, inspect typed payloads and modifiers,
verify direct declaration order and source offsets, and traverse both valid and
invalid blocks. A temporary adapter emits 46 canonical root, import, enum-case,
declaration, expression, statement, and block records; the cross-repository
test checks its shape and exact repeat-run determinism. Whole-graph validation,
including malformed range and child-topology rejection, is reserved for the
45.4 exit audit. No parser grammar is implemented by this checkpoint.

### 45.4 implementation record

`VerifiedSyntaxTree` is now the publication boundary for complete syntax. Its
factory invokes a whole-tree verifier before exposing the root. The verifier
checks the sealed arena, file identity, source ownership, nested half-open
ranges, nonempty valid spellings, import shapes, type flags, declaration and
switch structure, parent/child validity, typed-handle ownership, and agreement
between every expression or statement kind and its concrete payload. A visited
ordinal table terminates shared graph traversal without changing handle or
storage representation.

Ten subprocess fixtures preserve exact terminal `StateError` failures for
out-of-parent ranges, foreign expression, statement, and block handles,
expression, statement, and block payload mismatches, invalid children beneath
valid parents, spans backed by another source object, and impossible nullable-
element type syntax. A separate check repeatedly constructs, verifies, retains,
and replaces 256 complete trees while allocating additional managed arrays;
live roots and their arena-backed children remain usable through automatic
collections, and obsolete trees become unreachable between iterations.

The canonical 46-record adapter now verifies its tree before emission. Exact
records agree between direct compilation, serial Shuttle, a repeated serial
run, and parallel Shuttle. Native execution, wasm32 checking, warm reuse,
failure-output preservation, lexer parity, development and sanitizer suites,
Shuttle and editor tests, formatting, source style, documentation links, and
repository whitespace gates pass. Stage 45 is complete without parser grammar
or a compatibility change.

## 9. Compatibility and verification

Stage 45 retains artifact/compiler/runtime compatibility **7/6/10**,
process/receipt/manifest/toolchain schemas **2/1/1/1**, and compiler-paired
`cloth` **v0.4.0**. Shuttle and the standard library require no production
change.

Storage verification covers empty and nonempty arenas, every page boundary,
large multi-page inputs, stable typed handles, cross-owner and wrong-family
rejection, exact child order, empty and large child sequences, sealing, and
deterministic enumeration. Tree verification covers every node family,
half-open source ranges, source-backed spellings, optional versus invalid
children, member order, and malformed graph rejection.

The exit audit requires native x86-64 execution, wasm32 checking, development
and sanitizer suites, direct and Shuttle builds, serial/parallel equivalence,
warm reuse, failure-output preservation, formatting, documentation links, and
repository whitespace checks against the real `F:\Cloth` bootstrap project.

## 10. Non-goals

Stage 45 does not implement declaration, statement, or expression grammar;
semantic symbols; name or type resolution; HIR, MIR, LLVM lowering; constant
evaluation; a lossless or incremental tree; syntax rewriting; macros; editor
services; a language server; name interning; general collections or generics;
public compiler APIs; new Cloth syntax; or a compatibility-version change.
