# Proposal: Stage 38 portable text input and primitive parsing

Status: **complete — 38.4 exit audit passed 2026-09-06**.

This proposal adds one line-oriented standard-input API and strict conversion
from immutable Cloth strings to existing primitive values. It establishes a
trusted standard-library/runtime boundary without exposing foreign calls or
platform text conventions to applications.

See the [compiler roadmap](../../ROADMAP.md#stage-38-portable-text-input-and-primitive-parsing)
and [work ledger](../../TODO.md#stage-38-portable-text-input-and-primitive-parsing).

## 1. Source API

The initial input surface is one ordinary static standard-library member:

```cloth
import cloth.io::Console;

static func Main() throws IoError, ParseError {
  string line = Console.ReadLine() ??
      throw IoError("standard input reached EOF");
  int32 count = int32::parse(line);
  println(count);
}
```

Its exact source signature is
`static func ReadLine(): string? throws IoError`; its standard-library body
uses the private bridge defined below.

`Console` has canonical identity `cloth.io.Console`, is not part of the
`cloth.lang` prelude, and must be imported explicitly. `ReadLine` is public by
capitalization and is selected with ordinary declared-member access (`.`).
Stage 38 adds no public `Console` constructor or instance state.

Primitive parsing uses the existing language-owned meta namespace:

```text
T::parse(string text): T throws ParseError
```

`T` may be `bool`, `char`, `byte`, `int8`, `int16`, `int32`, `int64`,
`uint8`, `uint16`, `uint32`, `uint64`, `float32`, or `float64`. The aliases
`int`, `uint`, and `float` select `int32`, `uint32`, and `float32`
respectively. `parse` is lowercase because capitalization does not express
visibility for language-defined meta operations. `T::Parse`, a value-receiver
form such as `value::parse(text)`, nullable input, wrong arity, and nonprimitive
targets are compile-time errors.

The operation evaluates its non-null string argument exactly once and returns
the exact target type. It is a runtime operation and is not eligible in a
constant initializer. Existing Stage 34 effect checking applies: callers must
declare or infer `ParseError`, and `??` does not suppress a thrown parse error.

## 2. Standard-input semantics

`Console.ReadLine` reads the process's standard-input stream through the
following portable contract:

- A line ends at one U+000A line feed. The line feed is omitted. One U+000D
  immediately before that line feed is also omitted; every other carriage
  return is preserved.
- An empty terminated line returns a non-null empty string. End of stream before
  any code point for the next line returns `null` and is not an error.
- End of stream after content returns that final unterminated line. The next
  call returns `null`.
- Content, including leading and trailing whitespace, a byte-order mark,
  embedded U+0000, and all other valid scalars, is preserved without
  normalization or locale conversion.
- A read failure throws `IoError("could not read standard input")`. Malformed
  host text throws `IoError("standard input is not valid Unicode")`. A line
  whose UTF-8 representation cannot fit Cloth's `int32` string length throws
  `IoError("standard input line is too large")`.

On a Windows console, input is read as wide text and unpaired UTF-16 surrogates
are rejected. Redirected Windows input and POSIX input are byte streams that
must contain well-formed UTF-8. The active code page, C locale, environment,
and terminal locale do not affect decoding. A multibyte sequence may cross an
internal read boundary without changing the result.

The runtime retains unread content after a line feed for the next call and does
not close or reposition standard input. A read or decoding failure returns no
partial line. Allocation exhaustion and invalid internal runtime state retain
the existing deterministic terminal-failure behavior because constructing a
recoverable Cloth error cannot itself be guaranteed in those states.

## 3. Parsing grammar

Parsing consumes the complete string. It accepts no leading or trailing
whitespace, byte-order mark, comments, or trailing characters. All syntax
digits and punctuation are ASCII; Unicode digits and locale-specific signs,
grouping, decimal separators, boolean words, and case mappings are rejected.

### Integers

Signed targets accept an optional leading `+` or `-`; unsigned targets,
including `byte`, accept an optional `+` and reject `-` even for zero. The
remaining text is one of:

- a decimal digit run;
- lowercase `0b` followed by binary digits;
- lowercase `0o` followed by octal digits; or
- lowercase `0x` followed by hexadecimal digits.

Hexadecimal digits may use either case. Leading zeroes do not imply octal.
Digit separators follow the Stage 33 rule exactly: one underscore may occur
only between two digits in the same run. Every prefix requires a digit, the
complete mathematical value must fit the target, and the signed minimum is
accepted. Numeric suffixes such as `i32`, `u8`, and `f64` are rejected because
the meta target already states the type.

### Floating point

`float32::parse` and `float64::parse` accept an optional leading sign followed
by a decimal digit run, an optional fractional part with digits on both sides
of `.`, and an optional `e` or `E` exponent with an optional sign and required
decimal digit run. An integer-looking decimal such as `"12"` is accepted for a
floating target. Separators retain the Stage 33 placement rule.

Parsing rounds the exact decimal value once to the target IEEE-754 format using
round-to-nearest, ties-to-even. Finite values, signed zero, and representable
subnormals are accepted. A nonzero value that rounds to zero or infinity is out
of range. `nan`, `inf`, hexadecimal floating point, base-prefixed integers, and
type suffixes are not accepted as floating input. Results are independent of
the host floating environment and library implementation.

### Boolean and character

`bool::parse` accepts exactly `"true"` or `"false"`. `char::parse` accepts
exactly one Unicode scalar value, including U+0000 and non-BMP scalars. The
empty string, a combining sequence containing multiple scalars, and every
other multi-scalar string are invalid. Cloth strings are already valid UTF-8;
the runtime nevertheless rejects malformed internal string state rather than
reading beyond its explicit byte length.

Implementations scan in linear time with checked, bounded accumulators. They
must not call locale-sensitive conversion functions, stop at an embedded null,
or allocate storage proportional to a numeric exponent or mathematical
magnitude. Runtime input length remains bounded only by the existing `int32`
string representation; Stage 38 adds no smaller hidden parsing limit.

## 4. Typed failures

Stage 38 adds two ordinary public error classes beneath the recursive prelude:

```cloth
// cloth.lang.errors.IoError
error {
  IoError() {}
  IoError(string message): Error(message) {}
}

// cloth.lang.errors.ParseError
error {
  ParseError() {}
  ParseError(string message): Error(message) {}
}
```

Both inherit the compiler-owned `Error.Message` contract, remain extensible,
and are available without imports when the canonical `cloth` package is
present. `IoError` represents failures reported by portable I/O APIs;
`ParseError` represents malformed or unrepresentable primitive text. They are
not compiler-owned roots or runtime object layouts.

Every parse failure throws `ParseError` and quotes no attacker-controlled input:

- malformed text uses `invalid <type> text`; and
- a mathematically valid numeric value outside the target uses
  `<type> value is out of range`.

`<type>` is the canonical name: aliases therefore report `int32`, `uint32`, or
`float32`; `byte` remains distinct. A forbidden unsigned minus is malformed.
Boolean and character failures use only the malformed form.

The native runtime returns checked status and scalar bits. Generated code or
the source-defined standard-library wrapper constructs the appropriate error
through its ordinary source constructor. The runtime never allocates,
identifies, inherits from, or inspects `IoError` or `ParseError` directly.

## 5. Trusted library bridge

The compiler supplies a private implementation intrinsic only while compiling
the exact compiler-paired package `cloth`. It allows the `Console` wrapper to
request a checked line read. Primitive `parse` lowering uses the corresponding
checked runtime operation only when the verified canonical library supplies
the exact `ParseError` declaration.

The bridge is absent from ordinary application lookup, cannot be imported or
re-exported, and is not serialized as a public declaration. It cannot select
an arbitrary symbol, library, calling convention, address, or platform API.
Any internal bridge spelling and its physical lowering are implementation
details, not new Cloth keywords or user syntax. This is a narrow toolchain
privilege, not a general FFI.

Runtime ABI 6 adds exactly these exported operations:

```cpp
extern "C" void* cloth_rt_console_read_line(std::uint8_t* status) noexcept;
extern "C" std::uint8_t cloth_rt_parse_primitive(
    std::uint8_t kind, const void* text, std::uint64_t* bits) noexcept;
```

The input status values are `0` value, `1` end of stream, `2` I/O failure, `3`
invalid encoding, and `4` line too large. Only status `0` returns a non-null
managed string; an empty line is still a non-null string. Parse status values
are `0` value, `1` invalid text, and `2` out of range. Parse kind values are
ordered as `0` bool, `1` char, `2` byte, `3` int8, `4` int16, `5` int32, `6`
int64, `7` uint8, `8` uint16, `9` uint32, `10` uint64, `11` float32, and `12`
float64. Aliases use their canonical kind.

Primitive bits are canonical: booleans use zero or one, characters use a
Unicode scalar, signed integers use their fixed-width two's-complement bits,
unsigned integers use their fixed-width magnitude, and floats use IEEE-754
binary32 or binary64 bits. The parse operation writes zero bits on failure.
Unknown tags, null required pointers, malformed managed layouts, and impossible
status/pointer combinations are internal runtime failures.

Checkpoint 38.2 must land the complete ABI-6 input and parsing substrate before
any 38.3 compiler can emit a parsing requirement. This avoids two physically
different runtimes claiming the same ABI version.

## 6. Memory and evaluation

A successful line read returns a normal immutable managed UTF-8 `string`. Its
payload is copied into runtime-owned storage; no `FILE`, console buffer, host
pointer, or temporary decoding buffer is observable or borrowed. The value is
rooted before any subsequent safepoint. Internal buffering is native runtime
state and never becomes a Cloth object.

Parsing reads the managed string by explicit byte length and does not mutate or
retain it. Generated code keeps the input reachable throughout the call and
constructs `ParseError` only after a checked failure status. Success and failure
paths consume the argument once, publish no partially decoded value, and retain
the existing left-to-right evaluation and automatic error-propagation rules.

## 7. Packages, Shuttle, and compatibility

The public library additions advance the compiler-paired `cloth` package from
`0.2.0` to `0.3.0` in checkpoint 38.2. Exact version and digest continue through
the existing capability, toolchain-metadata, dependency, artifact, receipt,
cache, invalidation, and link paths. Direct `clothc` compilation remains
core-only unless the caller explicitly supplies the canonical library.

Shuttle inherits the child's standard input for `run` without reading,
decoding, buffering, logging, hashing, or forwarding it through a compiler
request. Input bytes and timing do not affect package artifacts, cache keys, or
the executable. `check` and `build` do not consume standard input. Existing
argument forwarding after `run --`, progress on standard error, application
streams, statuses, error reporting, failed-output preservation, and stale-run
prevention remain unchanged.

Stage 38 retains artifact format **5**, compiler ABI **5**, process protocol
**2**, receipt schema **1**, manifest schema **1**, and toolchain-metadata
schema **1**. Checkpoint 38.2 advances runtime ABI **5 to 6** and library version
**0.2.0 to 0.3.0**. Checkpoint 38.1 changes documentation only and leaves the
active values at 5/5/5, 2/1/1/1, and `cloth` v0.2.0.

Whole-project, independently compiled standard-library, separate-package, and
source-free consumers must agree. Interface and object artifacts use existing
callable, error-effect, dependency, and runtime-requirement records; no source
text, parse spelling, or platform stream state enters an artifact.

## 8. Diagnostics and verification

Compile-time diagnostics must cover invalid meta receivers, spelling, arity,
argument nullability/type, missing or incompatible `cloth` input, forged error
identities, malformed runtime requirements, and invalid internal result types.
Diagnostics remain source ordered and use canonical primitive names.

Implementation verification must cover:

- interactive-console and redirected input, LF/CRLF, lone CR, empty and final
  unterminated lines, repeated EOF, buffered multiple lines, whitespace, BOM,
  embedded null, non-BMP text, and chunk-split Unicode;
- strict POSIX UTF-8 and Windows UTF-16/redirected UTF-8 rejection, I/O failure,
  oversized-line checks, allocation failure policy, and no partial result;
- every canonical primitive and alias, exact boundaries and adjacent values,
  signed minima and zero, bases, separators, invalid digits/prefixes/suffixes,
  whitespace, embedded null, and full-input consumption;
- floating ties, signed zero, subnormals, extrema, nonzero underflow, overflow,
  extreme exponents, and host/locale independence;
- exact boolean spelling and zero, one, multiple, combining, BMP, non-BMP, and
  U+0000 character scalars;
- exact typed effects and messages, source-defined inheritance, once-only
  evaluation, automatic propagation, and GC during successful and failing paths;
- whole-project, separate-package, source-free, direct-native, and Shuttle
  equivalence; x86-64 native execution; and verified x86-64/wasm32 LLVM before
  and after optimization;
- exact standard-library invalidation and reuse, input-independent artifacts,
  relocated serial/parallel determinism, failure preservation, and stale-run
  prevention; and
- development, sanitizer, Rust/MSRV, editor, documentation, formatting, link,
  and repository gates across the compiler, runtime, Shuttle, standard library,
  and user documentation repositories.

## 9. Stage plan

1. **38.1 — Contract (complete).** Freeze source APIs, line and decoding
   semantics, parse grammar and rounding, typed failures, trusted bridge,
   memory, compatibility, diagnostics, verification, and non-goals.
2. **38.2 — Library and runtime foundation (complete).** Add `Console`, `IoError`, and
   `ParseError`; implement the private standard-library bridge and complete
   checked input/parsing substrate; advance runtime ABI to 6 and `cloth` to
   v0.3.0; and verify the low-level boundary independently.
3. **38.3 — Primitive parsing integration (complete).** Add every approved `T::parse`
   operation through semantic analysis, HIR/MIR, checked lowering, packages,
   native and cross-target behavior, Shuttle stream integration, editor support,
   and user documentation.
4. **38.4 — Exit audit (complete).** Close Unicode, grammar, rounding,
   resource, GC, compatibility, determinism, failure-preservation,
   native/cross-target, and repository quality matrices.

Stage 38 is complete. The separately authorized 38.4 audit closes every matrix
listed in Section 8 and records its concrete results in `docs/testing.md`.

## 10. Non-goals

Stage 38 does not add:

- character-at-a-time input, raw bytes, prompts, output APIs, stream objects,
  redirection controls, terminal control, asynchronous I/O, cancellation, or
  timeouts;
- files, directories, environment variables, clocks, networking, processes,
  serialization, or a general platform-services API;
- `tryParse`, default-on-failure conversion, parse-result objects, recovery or
  catch syntax, configurable radix, numeric suffix parsing, whitespace trimming,
  Unicode digits, locale rules, NaN/infinity text, or hexadecimal floats;
- string indexing, slicing, iteration, searching, normalization, case mapping,
  interpolation, or formatting;
- enum, struct, object, user-defined, array, nullable, arbitrary-precision,
  decimal, half, or complex parsing;
- public intrinsic/native/extern syntax, arbitrary FFI, native plugins, dynamic
  loading, build scripts, or application access to runtime symbols;
- static evaluation or general constant folding of parsed text;
- a WebAssembly runtime, WASI stream execution, or additional native targets;
  or
- independent standard-library version solving or any artifact, process,
  receipt, manifest, or toolchain-metadata schema change.
