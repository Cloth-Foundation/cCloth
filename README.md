# Cloth compiler - Stage 1.0 front end

This repository contains the deterministic Stage 1.0 front end for Cloth source
files (`.co`). It loads and lexes one source file, performs declaration and
definition parsing, and prints readable token and implicit file-class AST
summaries. Lexical and syntactic errors are reported with source ranges.

The project intentionally contains no type checker, backend, runtime, virtual
machine, garbage collector, or code-generation implementation yet.

## Requirements

- CMake 3.25 or newer
- A C++23 compiler (recent MSVC, Clang, or GCC)
- An optional build tool supported by CMake, such as Ninja

No third-party libraries are required.

## Configure and build

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

`--config Debug` is useful for multi-configuration generators and harmless for
single-configuration generators.

To make project warnings fail the build during development, configure with:

```sh
cmake -S . -B build -DCLOTH_WARNINGS_AS_ERRORS=ON
```

## Run

On Linux or macOS:

```sh
./build/clothc examples/User.co
```

On Windows with a single-configuration generator:

```powershell
.\build\clothc.exe examples\User.co
```

Visual Studio and other multi-configuration generators may place the binary in
`build/Debug`. The portable CMake target below builds the executable and runs
the example regardless of its output directory:

```sh
cmake --build build --config Debug --target run_compiler
```

The command accepts exactly one source path. Tokens are written to standard
output, diagnostics to standard error, and exit codes have these meanings:

- `0`: lexing and parsing completed without errors
- `1`: lexical or syntactic errors were reported
- `2`: command-line usage or source-loading failure

## Run tests

```sh
ctest --test-dir build --build-config Debug --output-on-failure
```

The internal test executables use no external test framework. Lexer coverage
includes tokens, comments, literals, operators, invalid input, ranges, and EOF.
Parser coverage includes declarations, visibility, constructors, overload
candidates, statements, expressions, source ranges, recovery, and deterministic
diagnostic ordering.

## VS Code

Install the recommended **C/C++** and **CMake Tools** extensions, open the
repository folder, and let CMake Tools configure the project.

Available tasks include:

- `CMake: configure`
- `CMake: build`
- `CMake: build and run compiler` (the default build task)
- `CMake: test`
- `CMake: check format`

For debugging, select `clothc` as the CMake launch target and choose the launch
configuration matching the local debugger: GDB, LLDB, or MSVC. Each passes
`examples/User.co` to the compiler and uses the repository root as its working
directory.

## Code style

The repository follows [CODE_STYLE.md](CODE_STYLE.md), a documented Google C++
style variant for C++23. `.clang-format` provides deterministic formatting and
`.clang-tidy` checks naming conventions. When `clang-format` is installed, CMake
also exposes `format` and `check_format` targets:

```sh
cmake --build build --target check_format
```

## Project structure

```text
include/cloth/          Public compiler interfaces
  source/               Source files and locations
  diagnostics/          Collected, presentation-independent diagnostics
  lexer/                Tokens and the lexer API
  parser/               Declaration and definition passes
  ast/                  Stable-handle syntax tree representation
  sema/                 Visibility and file-class declaration symbols
src/                    Implementations and the clothc driver
tests/                  Deterministic lexer, parser, and recovery tests
examples/User.co        Stage 1.0 implicit file-class example
docs/language_design.md Stable language and compiler design constraints
docs/grammar.md         Implemented Stage 1.0 grammar and precedence
.vscode/                Build, test, and debug integration
```

`SourceFile` owns source text. Token and AST names are `std::string_view`s into
that storage, so the `SourceFile` must outlive its tokens and parse result.
Moving a `SourceFile` does not invalidate those views.

Locations use a zero-based byte offset and one-based line and column numbers.
Columns count source bytes; tabs currently advance by one column. Both LF and
CRLF are treated as one line break. Identifiers currently use ASCII letters,
digits, and underscore; this makes the initial lexical contract explicit and
leaves Unicode identifier policy for a later language-design decision.

Source ranges are half-open (`[begin, end)`) and are retained on declarations,
parameters, types, blocks, statements, and expressions. The parser runs two
logical passes over the same immutable token stream: declaration discovery
first, followed by executable body parsing. AST recursion uses stable numeric
handles owned by `AstStorage`, keeping allocation strategy out of public node
identity.

## Extending the lexer

Keywords are defined in one table in `src/lexer/lexer.cc`, while token debug
names are centralized in `src/lexer/token.cc`. Numeric lexing currently
recognizes decimal integers and decimal fractions with digits on both sides of
the decimal point. Original lexemes are preserved so later compiler stages can
define numeric conversion, suffixes, separators, and alternate bases without
changing the token representation.

## Extending the parser

The implemented syntax and explicit precedence table are documented in
[docs/grammar.md](docs/grammar.md). Stable contextual rules, including implicit
file classes and capitalization-based visibility, are documented in
[docs/language_design.md](docs/language_design.md). Keep grammar changes and
parser tests in the same change, and keep contextual validation out of the
lexer.
