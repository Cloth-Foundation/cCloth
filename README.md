# Cloth compiler - Stage 0 lexer

This repository contains the first stage of the Cloth compiler: a standalone,
deterministic lexer for Cloth source files (`.co`). It loads one source file,
prints a readable token stream, reports lexical errors with source locations,
and returns a non-zero exit status when errors are found.

The project intentionally contains no parser, AST, semantic analysis, runtime,
or code-generation code yet.

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
./build/clothc examples/hello.co
```

On Windows with a single-configuration generator:

```powershell
.\build\clothc.exe examples\hello.co
```

Visual Studio and other multi-configuration generators may place the binary in
`build/Debug`. The portable CMake target below builds the executable and runs
the example regardless of its output directory:

```sh
cmake --build build --config Debug --target run_lexer
```

The command accepts exactly one source path. Tokens are written to standard
output, diagnostics to standard error, and exit codes have these meanings:

- `0`: lexing completed without errors
- `1`: the file was read, but lexical errors were reported
- `2`: command-line usage or source-loading failure

## Run tests

```sh
ctest --test-dir build --build-config Debug --output-on-failure
```

The internal test executable uses no external test framework. It covers empty
input, identifiers and keywords, primitive keywords, numeric literals,
punctuation, longest-match operators, comments, strings, character literals,
invalid input, locations, and the explicit EOF token.

## VS Code

Install the recommended **C/C++** and **CMake Tools** extensions, open the
repository folder, and let CMake Tools configure the project.

Available tasks include:

- `CMake: configure`
- `CMake: build`
- `CMake: build and run lexer` (the default build task)
- `CMake: test`
- `CMake: check format`

For debugging, select `clothc` as the CMake launch target and choose the launch
configuration matching the local debugger: GDB, LLDB, or MSVC. Each passes
`examples/hello.co` to the compiler and uses the repository root as its working
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
src/                    Implementations and the clothc driver
tests/                  Deterministic lexer tests
examples/hello.co       Example Cloth source
docs/language_design.md Stable language and compiler design constraints
.vscode/                Build, test, and debug integration
```

`SourceFile` owns source text. Token lexemes and location file names are
`std::string_view`s into that owned storage, so the `SourceFile` must outlive
its tokens. Moving a `SourceFile` does not invalidate those views.

Locations use a zero-based byte offset and one-based line and column numbers.
Columns count source bytes; tabs currently advance by one column. Both LF and
CRLF are treated as one line break. Identifiers currently use ASCII letters,
digits, and underscore; this makes the initial lexical contract explicit and
leaves Unicode identifier policy for a later language-design decision.

## Extending the lexer

Keywords are defined in one table in `src/lexer/lexer.cc`, while token debug
names are centralized in `src/lexer/token.cc`. Numeric lexing currently
recognizes decimal integers and decimal fractions with digits on both sides of
the decimal point. Original lexemes are preserved so later compiler stages can
define numeric conversion, suffixes, separators, and alternate bases without
changing the token representation.
