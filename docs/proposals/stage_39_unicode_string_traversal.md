# Proposal: Stage 39 Unicode string traversal

Status: **complete — 39.4 exit audit passed 2026-09-06**.

This proposal makes Cloth's immutable UTF-8 strings directly traversable while
preserving Unicode scalar values as the language's `char` unit. It closes the
existing mismatch between 32-bit `char` storage and byte-limited source and
artifact constants without exposing UTF-8 storage or introducing a general
iterator abstraction.

See the [compiler roadmap](../../ROADMAP.md#stage-39-unicode-string-traversal)
and [work ledger](../../TODO.md#stage-39-unicode-string-traversal).

## 1. Unicode scalar model

A Cloth `char` is exactly one Unicode scalar value: an integer in
`0..0x10ffff`, excluding the surrogate range `0xd800..0xdfff`. Its physical
representation remains an unsigned 32-bit value with four-byte size and
alignment. Not every 32-bit bit pattern is a valid `char`.

This definition already governs `char::parse`, string scalar counts, and
character printing. Stage 39 applies it consistently to source literals,
constant evaluation, artifacts, string indexing, and string iteration.
Unicode scalar values are not grapheme clusters, glyphs, UTF-8 bytes, or UTF-16
code units. Cloth performs no normalization, case conversion, or locale-based
interpretation.

## 2. Character and string escapes

A raw character literal contains exactly one well-formed UTF-8 scalar between
apostrophes:

```cloth
char ascii = 'C';
char combining = '́';
char thread = '🧵';
```

The combining-mark example is one scalar even though it may visually combine
with adjacent text. A base character followed by a combining mark is two
scalars and is therefore invalid as one character literal.

The existing simple escapes remain unchanged: `\n`, `\r`, `\t`, `\0`, `\\`,
`\'`, and `\"`. Stage 39 adds one Unicode escape form to both character and
string literals:

```text
unicode_escape = "\u{" hex_digit { hex_digit } "}" ;
```

The body contains one through six ASCII hexadecimal digits, accepts either
letter case, and permits leading zeroes. Signs, whitespace, underscores,
prefixes, empty bodies, more than six digits, missing braces, surrogates, and
values above `0x10ffff` are invalid. The `u` is lowercase. `\u{0}` denotes
U+0000 and is equivalent to `\0`.

In a character literal, the escape produces the one `char` value. In a string
literal, it appends that scalar's canonical UTF-8 encoding. Raw and escaped
spellings are semantically identical after decoding. Literal scanning and
decoding remain linear in source bytes and retain the existing bounded-token
policy.

## 3. String indexing

The existing index syntax accepts a non-null string receiver:

```cloth
string text = "A🧵Z";
char first = text[0];
char thread = text[1];
char last = text[2];
```

An index counts Unicode scalars from zero and must be assignable to `int32`,
matching array-index typing. The result has exact type `char` and is a value,
not a writable location. Assigning through `text[index]`, taking a storage path
through it, or otherwise attempting to mutate the immutable string is invalid.

The receiver is evaluated first and the index second, exactly once each. The
receiver remains reachable across index evaluation and the runtime access. A
nullable string must first be narrowed or asserted non-null; Stage 39 does not
add safe indexing.

An index is valid only when `0 <= index < text::length`. Failure terminates with
`cloth runtime error: string index is out of bounds`, following the existing
checked array-index contract. It is not a typed `throws` effect. Empty strings
therefore have no valid index. Bounds use the stored scalar count; the selected
value is decoded from the explicit byte length without reading through an
embedded U+0000.

Indexing is O(`text::byteLength`) in the worst case and uses constant auxiliary
space. The runtime does not add a hidden offset table or expose a byte index.

## 4. String iteration

The existing `for in` syntax also accepts a non-null string:

```cloth
for (var scalar in text) {
  println(scalar);
}

for (final char scalar in text) {
  println(scalar);
}
```

`var` infers `char`. An explicit iteration type uses ordinary assignment
compatibility from `char`; `final` retains its existing binding meaning. The
iteration variable is a fresh local value for each iteration, and reassigning a
non-final binding cannot change the string.

The string expression is evaluated exactly once before the loop and stays
reachable until the loop exits. Iteration visits scalars in source order using
a monotonically increasing UTF-8 byte cursor. The cursor is compiler/runtime
bookkeeping and cannot be observed or modified by Cloth code. An empty string
executes no body. `continue` advances to the next scalar, while `break` exits
without further decoding; return and error propagation retain their existing
control-flow rules.

A complete traversal is O(`text::byteLength`) with constant auxiliary space.
It must not lower to repeated scalar indexing, which could be quadratic. The
loop itself allocates no managed storage. Allocations and collections in the
body cannot invalidate the rooted string or cursor state.

## 5. Compiler representation and verification

The AST continues to use the existing index and `for in` syntax nodes. Semantic
analysis distinguishes array and string receivers: array indexing remains a
mutable element location, while string indexing is a `char` value. HIR records
the verified iterable/index operation kind so later stages never rediscover it
from syntax or guess from a forged type.

MIR uses dedicated verified string-scalar access and iteration operations. A
string iteration retains its byte cursor explicitly and may not be represented
as an array operation or as repeated index-from-zero access. MIR verification
checks exact string, `int32`, `char`, cursor, and control-flow relationships
before ABI lowering and again after optimization.

Literal decoding has one shared authoritative implementation for lexing,
constant evaluation, HIR verification, LLVM lowering, and artifact validation.
Downstream stages consume a canonical scalar value rather than independently
indexing the source lexeme.

Malformed compiler state, a forged non-scalar `char`, an invalid managed-string
layout, or a cursor that is negative, beyond the byte length, or not at a scalar
boundary is an internal failure. Such states never become replacement
characters or source-visible partial results.

## 6. Runtime ABI and memory ownership

String payloads remain immutable, runtime-owned UTF-8. Stage 39 does not expose
their address, bytes, cursor, allocation identity, or header layout. Logical
runtime support consists of:

```cpp
extern "C" std::uint32_t cloth_rt_string_scalar_at(
    const void* value, std::int32_t index) noexcept;
extern "C" std::uint8_t cloth_rt_string_next_scalar(
    const void* value, std::int32_t* byte_offset,
    std::uint32_t* scalar) noexcept;
```

`scalar_at` validates the non-null string layout and scalar index, then returns
the selected Unicode scalar. `next_scalar` accepts an offset initially set to
zero. It returns one after writing the current scalar and advancing the offset
to the next boundary, or zero only when the offset exactly equals the byte
length. On the end result it writes scalar zero and leaves the offset at the
end. Required pointers, offsets, boundaries, layout metadata, and returned
scalars are checked; impossible states terminate internally.

These operations add no managed allocation or safepoint. Generated code still
roots the receiver because evaluating the index and executing a loop body may
allocate before a later runtime access. Runtime ABI **7** adds both complete
operations together during checkpoint 39.3; no earlier checkpoint may emit an
ABI-7 requirement.

## 7. Packages, artifacts, and Shuttle

Artifact format 5 restricts a serialized character constant to `0..255`.
Checkpoint 39.2 therefore introduces artifact format **6**. A format-6
`character` value is a canonical decimal string representing `0..0x10ffff`
outside the surrogate range. Readers reject missing, negative, noncanonical,
surrogate, or oversized values. Format-5 artifacts are rejected and rebuilt,
never reinterpreted under the wider contract.

The `char` physical ABI remains an unsigned `i32`, so compiler ABI **5** and
native mangling remain unchanged. Checkpoint 39.3 advances runtime ABI **6 to
7**. Process protocol **2**, receipt schema **1**, manifest schema **1**, and
toolchain-metadata schema **1** remain unchanged. Capabilities and receipts
carry artifact format 6 through their existing fields.

The compiler-paired standard library adds no public declaration and remains
`cloth` v0.3.0. Its artifacts are rebuilt under format 6/runtime ABI 6 at 39.2
and runtime ABI 7 at 39.3 while retaining exact version, digest, dependency,
and compiler-identity selection. Direct, whole-project, separate-package, and
source-free consumers must agree on Unicode constants and traversal behavior.

Shuttle treats source, scalar values, iteration, and artifacts opaquely. It
updates only its compiler-owned compatibility expectations and fixtures while
preserving exact reuse, invalidation, atomic publication, relocated
serial/parallel determinism, failed-output preservation, and stale-run
prevention. Runtime string contents never enter cache keys or compiler
requests.

Checkpoint 39.2 advanced compatibility to artifact/compiler/runtime **6/5/6**.
Checkpoint 39.3 advances the active tuple to **6/5/7**. Schemas remain
**2/1/1/1** and `cloth` remains v0.3.0.

## 8. Diagnostics and verification

Source diagnostics must cover empty and multi-scalar character literals,
malformed raw UTF-8, unknown and malformed escapes, invalid hex digits, missing
braces, excess digits, surrogates, and values above U+10FFFF. They must recover
deterministically at the literal boundary without producing duplicate
downstream errors.

Index diagnostics must cover non-string/non-array receivers, nullable strings,
non-`int32`-compatible indices, and writes through string results. Iteration
diagnostics must cover nullable and non-iterable values, mismatched explicit
bindings, scope, and final assignment. Existing array wording and behavior
remain array-specific.

Implementation verification must cover:

- raw and escaped ASCII, U+0000, combining, BMP, non-BMP, and U+10FFFF scalars;
- overlong, truncated, surrogate, out-of-range, empty, multi-scalar, and every
  malformed Unicode-escape family in character and string literals;
- exact scalar bits through ordinary expressions, constants, HIR/MIR, LLVM,
  format-6 artifacts, imports, and source-free consumers;
- indexing empty, ASCII, mixed-width, embedded-null, and long strings at zero,
  the final index, negative one, length, and adjacent bounds;
- receiver-before-index and exactly-once evaluation, GC during index
  evaluation, and no later side effect after a terminal bounds failure;
- inferred, explicit, and final string iteration; empty and mixed-width input;
  break, continue, return, error propagation, nesting, shadowing, and scope;
- one-time iterable evaluation, collection in loop bodies, linear long-string
  traversal, and rejection of malformed HIR, MIR, runtime layouts, and cursors;
- unchanged array indexing and iteration, string equality, concatenation,
  lengths, printing, parsing, input, nullability, and optimizer behavior;
- artifact-format-6/compiler-ABI-5/runtime-ABI-7 capability, golden,
  malformed-artifact, rebuild, link, exact-reuse, affected-invalidation,
  failure-preservation, and relocated determinism matrices;
- verified x86-64 and wasm32 LLVM before and after optimization, native x86-64
  execution, and whole/separate/source-free/direct/Shuttle equivalence; and
- development, sanitizer, Rust/MSRV, editor, user and maintainer documentation,
  formatting, links, whitespace, and repository quality gates.

## 9. Stage plan

1. **39.1 — Contract (complete).** Freeze the Unicode scalar model, literal and
   escape grammar, indexing and iteration semantics, complexity, runtime/GC
   ownership, compatibility transitions, diagnostics, verification, and
   non-goals.
2. **39.2 — Scalar frontend and artifacts (complete).** Implement authoritative literal
   decoding, full-range scalar constants, semantic/HIR verification, LLVM
   values, artifact format 6, package integration, editor grammar, and focused
   user documentation without enabling string traversal.
3. **39.3 — Traversal and lowering (complete).** Implement typed string indexing and
   iteration through verified HIR/MIR, runtime ABI 7, optimized LLVM, GC-safe
   native execution, packages, source-free consumers, Shuttle, and user
   documentation.
4. **39.4 — Exit audit (complete).** Close Unicode, bounds, evaluation,
   complexity, GC, malformed-state, compatibility, determinism,
   failure-preservation, native/cross-target, and repository quality matrices.

Checkpoints 39.1 through 39.4 are complete. The exit audit covers every literal,
bounds, evaluation, control-flow, complexity, GC, malformed-state,
compatibility, determinism, failure-preservation, native/cross-target, and
repository matrix in this contract. Active compatibility remains
artifact/compiler/runtime **6/5/7**, schemas **2/1/1/1**, and `cloth` v0.3.0.
Checked scalar indexing and linear string iteration are enabled across native,
package, source-free, and Shuttle paths.

## 10. Non-goals

Stage 39 does not add:

- grapheme-cluster, glyph, UTF-8 byte, UTF-16 code-unit, locale, or normalized
  indexing and iteration;
- string mutation, writable character locations, borrowed views, offset tables,
  interning, or observable storage;
- slicing, ranges, searching, containment, splitting, replacement, trimming,
  normalization, case conversion, interpolation, or formatting;
- normal string methods or a source-defined standard-library string API;
- general iterators, iterable interfaces, generators, destructuring,
  asynchronous iteration, collections, slices, or generics;
- safe indexing, nullable value types, an `IndexError`, local error recovery, or
  migration of array bounds failures into typed errors;
- character arithmetic, numeric conversions, boxing, reflection beyond existing
  type names, or new equality semantics;
- static execution, compile-time string traversal, loop unrolling, or new
  optimizer controls;
- a WebAssembly runtime, WASI strings, new native targets, public FFI, or host
  string APIs; or
- a compiler-ABI, process-protocol, receipt, manifest, toolchain-metadata, or
  standard-library-version change.
