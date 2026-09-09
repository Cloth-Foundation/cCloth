# Stage 44: Self-hosted lexer parity

Stage 44 moves lexical analysis into the Cloth bootstrap compiler at
`F:\Cloth`. The C++23 lexer in `cCloth` remains the behavior oracle until the
stage exit audit passes. This is compiler implementation work; it adds no Cloth
source syntax or public standard-library API.

See the [compiler roadmap](../../ROADMAP.md#stage-44-self-hosted-lexer-parity)
and [work ledger](../../TODO.md#stage-44-self-hosted-lexer-parity).

## 1. Parity boundary

For the same exact source bytes and display path, the C++ and Cloth lexers must
produce the same ordered observations:

- token kinds;
- token byte spans and half-open source ranges;
- EOF placement;
- lexical diagnostic categories and source ranges; and
- recovery boundaries after malformed input.

The C++ token spelling is a non-owning view into its `SourceFile`. The Cloth
equivalent is a source-backed `SourceSpan`, not an eagerly copied `string`.
Parity compares the bytes covered by each span. Parser, semantic, HIR, MIR,
LLVM, runtime, and command-line diagnostic rendering behavior are outside this
stage.

The token-kind set and spellings in `include/cloth/lexer/token.h` and
`src/lexer/token.cc` are the initial oracle. Every keyword, punctuation token,
operator, and literal category must have a matching Cloth `TokenKind`. A parity
fixture will fail when either list changes without the other.

## 2. Input, cursor, and source ranges

`frontend.source::SourceFile` owns the exact `byte[]` loaded through
`File.ReadBytes`. Lexing never normalizes the path, decodes the entire file,
adds a terminator, rewrites newlines, or mutates the array.

Offsets are zero-based byte offsets represented as `int32`; Stage 43's 64 MiB
limit keeps every valid offset and the one-past-end position representable.
Lines and columns are one-based `uint32` values. Columns count source bytes,
matching the C++ bootstrap; they are not Unicode-scalar or display columns.

The cursor advances as follows:

- carriage return increments the line and resets the column to one;
- line feed immediately after carriage return resets the column without
  incrementing the line again;
- a standalone line feed increments the line and resets the column to one; and
- every other byte increments the column by one.

Token and diagnostic ranges are half-open `[begin, end)`. EOF has a zero-length
range at the final cursor position. `SourceSpan` holds a `SourceFile` plus its
half-open byte offsets and exposes checked byte access and exact ASCII matching;
it does not promise UTF-8 decoding or an owned string.

## 3. Lexical grammar

The self-hosted lexer preserves the current C++ rules exactly:

- identifiers are ASCII `[A-Za-z_][A-Za-z0-9_]*` and are reclassified against
  the complete canonical keyword table;
- ignored whitespace is space, tab, carriage return, and line feed;
- `//` comments end before a line break and `/* ... */` comments are
  non-nesting;
- punctuation and operators use longest-match selection;
- decimal, binary, octal, and hexadecimal numeric spellings preserve current
  prefix, separator, exponent, and suffix validation;
- string and character tokens preserve their original source bytes and validate
  the current simple escapes, `\u{...}` escapes, canonical UTF-8, and character
  scalar count; and
- an unexpected byte produces one diagnostic, consumes that byte, and resumes.

Raw non-ASCII bytes do not become identifier characters during Stage 44. They
are accepted only where the existing string and character literal rules accept
valid UTF-8. Unicode identifiers require a later language contract.

## 4. Diagnostics and recovery

`LexDiagnosticKind` records stable machine-comparable categories. Each
`LexDiagnostic` also retains the source range needed to reproduce the C++
diagnostic and, when applicable, a source span containing the rejected spelling
or byte. The initial categories cover:

- unexpected bytes and unterminated block comments;
- unterminated or malformed string and character literals;
- invalid UTF-8, unknown escapes, invalid Unicode escapes or scalar values,
  empty characters, and multi-scalar characters; and
- invalid numeric cores or suffixes, missing base or exponent digits, invalid
  base digits or prefixes, invalid separators, and incompatible suffix forms.

The lexer continues after recoverable errors at the same boundary as `cCloth`
and always emits exactly one final EOF token. Diagnostics remain in source order
and are never duplicated by the sizing pass. Stage 44 stores structured
diagnostics; human-readable CLI rendering belongs to the later bootstrap driver
work and is not approximated with ad hoc strings.

## 5. Bounded storage and execution

The absence of a growable collection must not force a worst-case
`source_length + 1` token or diagnostic allocation. `Lexer.Lex` therefore uses
one scanner implementation in two deterministic passes:

1. the measure pass scans all bytes and counts tokens and diagnostics; and
2. the emit pass rescans the unchanged `SourceFile` into exact-sized buffers.

Both passes use fresh cursors and identical token-boundary logic. The measure
pass does not publish objects. The emit pass must fill exactly the measured
counts; a mismatch is an internal compiler failure, not a partial result.
Runtime-sized arrays remain fixed after construction. `LexerResult` owns the
complete `TokenBuffer` and `LexDiagnosticBuffer`, which cannot be observed until
both passes finish.

Every loop advances the byte cursor or terminates. Scanner lookahead is checked
before access, integer counts are checked before increment, and token count is
bounded by `source_length + 1`. The only expected asymptotic cost is two linear
passes over the source plus linear validation of the token spellings they
cover; no token operation may rescan the full file.

## 6. Source organization

The bootstrap source is divided by compiler responsibility:

```text
src/
  Main.co
  frontend/
    diagnostic/
      LexDiagnostic.co
      LexDiagnosticBuffer.co
      LexDiagnosticKind.co
    lexer/
      Lexer.co
      LexerResult.co
      ByteClass.co
      KeywordClassifier.co
      LexSink.co
      Utf8Decoder.co
      scanners/
        IdentifierScanner.co
        NumberScanner.co
        OperatorScanResult.co
        OperatorScanner.co
        TextScanner.co
        TriviaScanner.co
    source/
      SourceCursor.co
      SourceFile.co
      SourceLocation.co
      SourceRange.co
      SourceSpan.co
    token/
      Token.co
      TokenBuffer.co
      TokenKind.co
```

This tree is a responsibility map, not permission to add empty placeholders.
Files are created only with their checkpoint implementation. `Lexer.co`
coordinates passes and scanners; it does not accumulate keyword tables,
numeric validation, UTF-8 decoding, storage, CLI output, or unrelated helpers.
Scanner files own complete lexical domains and share only the cursor, source,
sink, and byte-class contracts.

Each `.co` file defines one primary implicit type with the same name as its file
stem. Imports are explicit and sorted. Dependencies point from the lexer toward
source, token, and diagnostic data; those lower layers do not import the lexer.
The `scanners` directory is internal compiler structure even though Cloth's
current capitalization visibility rule requires cross-file helper operations to
be public. No `Utils`, `Common`, `Misc`, or catch-all helper type is permitted.

`F:\Cloth\STYLE.md` owns the bootstrap's formatting and naming rules, and
`F:\Cloth\ARCHITECTURE.md` owns the durable source map. The C++ oracle remains
governed by `cCloth/CODE_STYLE.md` and Google C++ Style.

## 7. Checkpoint plan

1. **44.1 — Contract and source architecture (complete).** Freeze parity,
   byte/range rules, recovery, bounded two-pass storage, source-backed tokens,
   file boundaries, compatibility, verification, and non-goals.
2. **44.2 — Scanner foundation (complete).** Implement source spans and cursors,
   exact-sized result storage, byte classification, trivia, identifiers,
   keywords, punctuation, operators, EOF, and focused bootstrap execution.
3. **44.3 — Literal completion (complete).** Implement numeric spellings,
   strings, characters, UTF-8 and escape validation, structured lexical
   diagnostics, recovery, and complete tokenization without parser coupling.
4. **44.4 — Differential parity and exit audit (complete).** Compare the Cloth and C++
   lexers across canonical fixtures, generated byte inputs, malformed recovery,
   real compiler files, both targets, native execution, Shuttle reuse, and all
   repository quality gates.

Stage 44 completed on 2026-09-08 after the separately authorized 44.4 audit.

## 8. Compatibility and ownership

Stage 44 changes only source inside the bootstrap compiler and test adapters
used to compare it with `cCloth`. Compatibility remains artifact/compiler/
runtime **7/6/10**, process/receipt/manifest/toolchain schemas **2/1/1/1**, and
the compiler-paired `cloth` package **v0.4.0**.

No bootstrap token, source, scanner, or diagnostic type enters a package
artifact, compiler ABI, runtime ABI, Shuttle protocol, or standard-library API.
The standard library continues to own only `File.ReadBytes`; Shuttle remains
opaque to source bytes and lexer behavior.

## 9. Verification

Focused tests cover empty and trivia-only sources, every keyword and token,
longest-match operator prefixes, comment termination, CR/LF/CRLF positions,
numeric forms, text escapes, valid and malformed UTF-8, unexpected byte
recovery, EOF placement, exact allocation counts, and cursor boundaries at
empty, final-byte, and maximum valid offsets.

Differential tests compare canonical records rather than presentation text.
Each record contains a token or diagnostic category, byte span, begin/end line
and column, and exact covered bytes. Test adapters are not public compiler CLI
or build-protocol surfaces. Fixed fixtures, the existing C++ lexer tests,
generated bounded byte sequences, and every `.co` file in `F:\Cloth` form the
parity corpus. Failures report the first differing record and input offset.

The 44.4 exit audit also requires development and sanitizer compiler suites,
x86-64 and wasm32 verification, native bootstrap execution, deterministic
direct and Shuttle builds, exact warm reuse, failure-output preservation,
formatting, documentation links, and repository whitespace checks.

## 10. Non-goals

Stage 44 does not add a parser, AST, name interning, Unicode identifiers,
preprocessing, semicolon insertion, nested block comments, interpolation,
incremental lexing, editor services, a language server, general collections,
streaming source input, source decoding APIs, public token APIs, or a new
language, runtime, package, or Shuttle compatibility boundary.
