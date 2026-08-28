# Cloth compiler - Stage 12.3.5 null ergonomics

This repository contains the deterministic Stage 12.3.5 compiler core for Cloth
source files (`.co`). It discovers a path-derived package graph, lexes and
parses its implicit file classes, checks imports, arrays, types, and visibility,
verifies typed HIR, analyzes control flow, and lowers executable definitions to
target-independent MIR, a verified target ABI, and textual LLVM IR. The driver
prints readable token, AST, HIR, MIR, and ABI summaries or emits a standalone
LLVM module or builds a native x86-64 executable. Errors are collected with
source ranges.

The project includes structured `while` and array `for` iteration, `break` and
`continue` control flow, fixed-length mutable arrays with checked indexing, and
a minimal native runtime for allocation, strings, null checks, object type
descriptors, and typed `print` and `println` overloads. It intentionally
contains no garbage collector, virtual machine, standard library, debugger, or
external package registry. LLVM IR emission has no link-time dependency on LLVM
libraries.

## Requirements

- CMake 3.25 or newer
- A C++23 compiler (recent MSVC, Clang, or GCC)
- An optional build tool supported by CMake, such as Ninja
- Optional LLVM `opt` for LLVM IR verification tests
- LLVM `llc` and the configured C++ linker driver for native builds

No third-party libraries are required.

## Configure and build

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

`--config Debug` is useful for multi-configuration generators and harmless for
single-configuration generators.

The checked-in development preset provides the same workflow with tests and
warnings-as-errors enabled in `build/dev`:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Sanitizer, coverage, and Clang/libFuzzer presets are also available. Their
compiler requirements and commands are documented in
[docs/testing.md](docs/testing.md).

To make project warnings fail the build during development, configure with:

```sh
cmake -S . -B build -DCLOTH_WARNINGS_AS_ERRORS=ON
```

## Run

On Linux or macOS:

```sh
./build/clothc examples/User.co examples/Repository.co
```

On Windows with a single-configuration generator:

```powershell
.\build\clothc.exe examples\User.co examples\Repository.co
```

Select the 32-bit WebAssembly layout with:

```sh
./build/clothc --target=wasm32 examples/User.co examples/Repository.co
```

Emit LLVM IR to standard output or a file with:

```sh
./build/clothc --emit-llvm examples/User.co examples/Repository.co
./build/clothc --emit-llvm=cloth.ll examples/User.co examples/Repository.co
```

Build and run Cloth's first native program with:

```sh
./build/clothc --build=hello examples/hello/HelloWorld.co
./hello
```

On Windows, use `--build=hello.exe` and run `.\hello.exe`.

The Stage 7 loop and typed-output example is FizzBuzz:

```sh
./build/clothc --build=fizzbuzz examples/FizzBuzz.co
./fizzbuzz
```

The Stage 9 array example sums a collection through `Length` and indexing:

```sh
./build/clothc --build=array-sum examples/ArraySum.co
./array-sum
```

Stage 10 adds inferred and explicitly typed iteration declarations:

```sh
./build/clothc --build=for-each examples/ForEach.co
./for-each
```

```cloth
for (var value in values) { ... }
for (int32 value in values) { ... }
```

A project uses an empty or metadata-only `cloth.toml` and a `src/` directory.
Compile only its entry file; imports and same-package sources are discovered:

```sh
./build/clothc --build=imports \
  tests/projects/imports/src/Main.co
./imports
```

Imports are identifier paths rather than strings:

```cloth
import models::User;
import services.api::*;
import legacy::User as LegacyUser;
```

Visual Studio and other multi-configuration generators may place the binary in
`build/Debug`. The portable CMake target below builds the executable and runs
the example regardless of its output directory:

```sh
cmake --build build --config Debug --target run_compiler
```

The command accepts `--target=x86_64` or `--target=wasm32`, one output mode,
and one or more source paths as one compilation set. Output modes are
`--emit-llvm[=<path>]` and `--build=<path>`. Native builds currently require
the x86-64 target.
Without LLVM emission, tokens, ASTs, typed HIR, control-flow MIR, and the
portable ABI are written to standard output. Diagnostics are written to
standard error. Exit codes have these meanings:

- `0`: front-end compilation completed without errors
- `1`: lexical, syntactic, or semantic errors were reported
- `2`: command-line usage or source-loading failure

## Run tests

```sh
ctest --test-dir build --build-config Debug --output-on-failure
```

The internal test executables use no external test framework. Lexer coverage
includes tokens, comments, literals, operators, invalid input, ranges, and EOF.
Parser coverage includes imports, declarations, arrays, `for` bindings,
visibility, constructors, overload candidates, statements, expressions, source
ranges, and recovery.
Semantic coverage includes package and cross-file binding, aliases, wildcards,
privacy, core types, exact overload and constructor resolution, lexical scopes,
type checking, array inference and access, return paths, portable file-name
collisions, typed HIR, and deterministic diagnostics. MIR coverage
includes branches, fallthrough joins, structured loop edges, short-circuit phi
nodes, dead blocks, field initializers, array operations, iteration latches,
explicit conversions, receivers, and verifier failures.
ABI coverage includes primitive and reference layouts, class padding, both
target widths, receiver slots, constructor returns, linkage, mangling, and
verifier failures.
Backend coverage includes arithmetic, short-circuit branches, phi values,
objects, arrays, field initializers, receiver forms, constructors, typed output,
and wasm32.
When `opt` is available, CTest also verifies an emitted module with LLVM itself.
When `llc` is available, CTest builds and executes the native examples and the
multi-package project. Their output is compared exactly against golden files.
Every CTest case has a configurable timeout, and native program probes have a
shorter subprocess timeout. Opt-in coverage, sanitizer, and lexer/parser fuzz
configurations are documented in [docs/testing.md](docs/testing.md).

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
`examples/User.co` and `examples/Repository.co` to the compiler and uses the
repository root as its working directory.

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
CMakeLists.txt          Top-level project and component orchestration
CMakePresets.json       Development, sanitizer, coverage, and fuzz presets
cmake/                  Shared options, tooling, and test helpers
include/cloth/          Public compiler interfaces
  source/               Source files and locations
  diagnostics/          Collected, presentation-independent diagnostics
  lexer/                Tokens and the lexer API
  parser/               Declaration and definition passes
  ast/                  Stable-handle syntax tree representation
  sema/                 Stable types, symbols, binding, and type checking
  hir/                  Typed target-independent intermediate representation
  flow/                 Callable-level control-flow analysis
  mir/                  Explicit basic-block intermediate representation
  target/               Backend-neutral target data-layout descriptions
  abi/                  Object layout, signatures, linkage, and mangling
  backend/              LLVM IR emission
  compiler/             Multi-file compilation orchestration
  project/              Project-root and source-root discovery
  runtime/              Native runtime ABI interface
src/                    Compiler implementation, clothc, and owned CMake target
runtime/                Native runtime implementation and owned CMake target
tests/                  Test targets, fixtures, projects, and CTest registration
examples/               Native and cross-file language examples
docs/language_design.md Stable language and compiler design constraints
docs/grammar.md         Implemented grammar and precedence
docs/semantic_analysis.md Implemented Stage 2.0 semantic rules
docs/control_flow_and_mir.md Implemented Stage 3.0 IR contract
docs/data_layout_and_abi.md Implemented Stage 4.0 ABI contract
docs/llvm_backend.md     Implemented Stage 5.0 LLVM lowering contract
docs/native_runtime.md   Implemented Stage 6.0 native execution contract
docs/packages_and_imports.md Implemented Stage 8.0 package graph contract
docs/arrays_and_indexing.md Implemented Stage 9.0 array contract
docs/array_iteration.md   Implemented Stage 10.0 iteration contract
docs/testing.md           Stage 10.1 test and diagnostic-build contract
docs/printing_and_object_representation.md Stage 10.5 output contract
docs/void_and_callable_contracts.md Stage 11 void contract
docs/final_bindings.md   Stage 12.1 single-assignment contract
docs/static_members.md   Stage 12.2 static ownership and entry contract
docs/nullability.md      Stage 12.3.5 nullable reference and operator contract
.vscode/                Build, test, and debug integration
```

`SourceFile` owns source text. Token and AST names are `std::string_view`s into
that storage, so the `SourceFile` must outlive its tokens and parse result.
Moving a `SourceFile` does not invalidate those views. `Compilation` owns all
three for multi-file front-end runs.

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

Semantic analysis registers every file class before members and every member
before bodies. It emits stable `FileId`, `TypeId`, and `SymbolId` handles, then
lowers bound syntax to typed HIR. See
[docs/semantic_analysis.md](docs/semantic_analysis.md) for the implemented
contract and deliberate Stage 2 boundaries.

HIR is verified before control-flow analysis. Callable bodies and field
initializers then lower to verified MIR with explicit instructions, blocks,
terminators, short-circuit branches, and phi values. See
[docs/control_flow_and_mir.md](docs/control_flow_and_mir.md) for the Stage 3
contract and backend boundary.

Verified MIR lowers to target-specific, LLVM-friendly ABI descriptions without
linking LLVM into the front end. Stage 4 fixes primitive representation, object
layout, linkage, receiver slots, constructor results, and versioned symbol
mangling. See [docs/data_layout_and_abi.md](docs/data_layout_and_abi.md).

The Stage 5 backend consumes only verified MIR and ABI data. It emits opaque-
pointer LLVM IR, composes field initializers with constructors, and isolates
allocation and null-receiver behavior behind runtime intrinsics. See
[docs/llvm_backend.md](docs/llvm_backend.md).

Stage 6 binds the first typed core output intrinsic, emits a native `main`
adapter for Cloth's public static `Main()`, implements the minimal runtime, and drives
LLVM object emission plus the configured host linker. Stage 7 adds structured
loops and `String`, `int32`, and `bool` output overloads, making FizzBuzz the
first complete control-flow example. See
[docs/native_runtime.md](docs/native_runtime.md).

Stage 8 derives package identities from paths, discovers projects through
`cloth.toml`, closes the source graph recursively, and implements explicit,
wildcard, aliased, same-package, and cyclic imports. See
[docs/packages_and_imports.md](docs/packages_and_imports.md).

Stage 9 adds homogeneous `T[]` references, array literals, mutable checked
indexing, and the public `Length` member. Arrays lower through explicit HIR and
MIR nodes to a garbage-collector-ready runtime boundary. See
[docs/arrays_and_indexing.md](docs/arrays_and_indexing.md).

Stage 10 adds `for (declaration in expression)` over arrays. The loop binding
may infer its element type with `var` or state it explicitly. Lowering evaluates
the iterable once and uses a dedicated latch so `continue` always advances the
hidden index. See [docs/array_iteration.md](docs/array_iteration.md).

Stage 10.1 hardens that contract with malformed-header recovery, exact MIR edge
checks, nested-loop and terminating-body coverage, native reference iteration,
copy-semantics and evaluate-once probes, runtime guards, timeouts, coverage
instrumentation, sanitizers, and an opt-in lexer/parser fuzzer. See
[docs/testing.md](docs/testing.md).

Stage 10.5 completes primitive `print` overloads, adds `println(value)` and
`println()`, and initializes file-class object headers with opaque type
descriptors. Default object output is the stable `<qualified.Type>` form and
never includes an address. See
[docs/printing_and_object_representation.md](docs/printing_and_object_representation.md).

Stage 11 adds explicit `void`, defaults omitted function returns to the same
canonical type, and prevents void calls from being used as values. It preserves
valueless fallthrough and lowers explicit and implicit forms identically. See
[docs/void_and_callable_contracts.md](docs/void_and_callable_contracts.md).

Stage 12.1 adds `final` fields and bindings, inferred `var` locals, and
constructor-aware definite initialization. Final affects rebinding rather than
object mutability and has no ABI representation. See
[docs/final_bindings.md](docs/final_bindings.md).

Stage 12.2 adds receiver-free static functions, constant scalar static fields,
and explicit `static func Main()`. Static members belong to the implicit file
class but not to any instance; static fields are excluded from object layout,
and static functions have no `self` binding or receiver ABI slot. See
[docs/static_members.md](docs/static_members.md).

Stage 12.3.1 makes references non-null by default and adds `T?` as a distinct
nullable semantic type. Array nullability composes as `T?[]`, `T[]?`, and
`T?[]?`; MIR keeps explicit widening conversions while the ABI erases the
qualifier to the existing opaque pointer representation. See
[docs/nullability.md](docs/nullability.md).

Stage 12.3.2 applies directional nullable compatibility consistently to
initializers, assignments, calls, returns, arrays, and iteration bindings.
Unsafe dereference, indexing, iteration, and non-null argument passing are
rejected before lowering.

Stage 12.3.3 closes the construction gap: every non-null reference field must
be definitely initialized on every constructor exit. Reads, `self` escape, and
instance calls that could observe an uninitialized field are rejected. Mutable
fields may still be reassigned after initialization; final fields retain their
exactly-once contract.

Stage 12.3.4 narrows nullable locals and parameters after direct `null` checks.
True and false facts compose through `!`, `&&`, `||`, branches, and guard
clauses; assignments invalidate them. Narrowed reads retain explicit HIR and
MIR type evidence while erasing to the unchanged reference ABI.

Stage 12.3.5 adds nullable presence conditions, safe reference-field access
with `?.`, lazy null coalescing with `??`, and postfix non-null assertion with
`!`. Assertions use an explicit runtime guard and trap on null; safe access and
coalescing evaluate their left operand once and lower to short-circuit MIR.

## Extending the lexer

Keywords are defined in one table in `src/lexer/lexer.cc`, while token debug
names are centralized in `src/lexer/token.cc`. Numeric lexing currently
recognizes decimal integers and decimal fractions with digits on both sides of
the decimal point. Original lexemes are preserved so later compiler stages can
define numeric conversion, suffixes, separators, and alternate bases without
changing the token representation.

Function declarations use the `func` keyword. The former `function` spelling is
an ordinary identifier and is not accepted as declaration syntax.

## Extending the parser

The implemented syntax and explicit precedence table are documented in
[docs/grammar.md](docs/grammar.md). Stable contextual rules, including implicit
file classes and capitalization-based visibility, are documented in
[docs/language_design.md](docs/language_design.md). Keep grammar changes and
parser tests in the same change, and keep contextual validation out of the
lexer.
