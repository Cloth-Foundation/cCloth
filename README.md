# Cloth

Cloth is a statically typed, compiled language designed around a
write-once, use-anywhere model. Its compiler is written in C++23 and lowers
verified, target-independent intermediate representations to textual LLVM IR.
The native runtime provides managed objects, arrays, immutable UTF-8 strings,
and precise garbage collection.

```cloth
// HelloWorld.co
static func Main() {
  println("Hello, World!");
}
```

Every `.co` file is an implicit class named after the file. `User.co`
therefore defines `User`; fields, functions, constructors, and nested types
belong to that class without an enclosing `class User` declaration.
Capitalization carries visibility: names beginning with an uppercase ASCII
letter are public, while lowercase and underscore-prefixed names are private.

Cloth is under active development. The language, compiler interfaces, runtime
ABI, and tooling are not yet stable, and there is currently no standard
library or external package registry.

## What works today

The compiler has a deterministic lexer and two-pass parser, semantic analysis,
typed HIR, control-flow MIR, a verified target ABI, and an LLVM IR backend. It
supports path-derived packages and imports, functions and constructors,
structured control flow, arrays, null-safe managed references, strings,
objects, single inheritance, overriding, virtual dispatch, and direct-base
calls.

The backend can emit LLVM IR for x86-64 and wasm32 layouts. Native executable
generation currently targets x86-64 and uses LLVM `llc` plus the configured
C++ linker driver.

## Build from source

Requirements:

- CMake 3.25 or newer
- Ninja for the checked-in CMake presets
- A C++23 compiler such as recent Clang, GCC, or MSVC
- LLVM `llc` and a C++ linker driver to build native Cloth executables
- Optional LLVM `opt` for backend verification tests

No third-party C++ libraries are required. From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The development preset creates `build/dev`, enables tests, and treats compiler
warnings as errors. See [Testing](docs/testing.md) for sanitizer, coverage, and
fuzzing presets.

## Compile a Cloth program

On Linux or macOS:

```sh
./build/dev/clothc --build=build/dev/hello \
  examples/hello/HelloWorld.co
./build/dev/hello
```

On Windows PowerShell:

```powershell
.\build\dev\clothc.exe --build=build/dev/hello.exe `
  examples\hello\HelloWorld.co
.\build\dev\hello.exe
```

Emit LLVM IR instead of a native executable with:

```sh
./build/dev/clothc --emit-llvm=build/dev/hello.ll \
  examples/hello/HelloWorld.co
```

Without an output option, `clothc` prints its tokens, AST, HIR, MIR, and ABI
summaries. Diagnostics go to standard error. The complete command shape is:

```text
clothc [--target=x86_64|wasm32]
       [--emit-llvm[=<path>] | --build=<path>]
       <source.co>...
```

Native `--build` output currently requires `--target=x86_64`.

## Projects and imports

A multi-file project has an empty or metadata-only `cloth.toml` and a
`src/` source root:

```text
my_app/
  cloth.toml
  src/
    Main.co
    models/
      User.co
```

Compile an entry file; Cloth discovers imported files and same-package sources
from the project root:

```sh
./build/dev/clothc --build=build/dev/my_app my_app/src/Main.co
```

Imports use identifier paths rather than file-name strings:

```cloth
import models::User;
import services.api.*;
import legacy::User as LegacyUser;
```

See [Packages and imports](docs/packages_and_imports.md) for path identity,
visibility, discovery, and collision rules.

## Propose a change with an RFC

Open an RFC before implementing a change to source syntax, language semantics,
the public compiler interface, object layout, the runtime ABI, or observable
tooling behavior. Focused bug fixes, tests, documentation corrections, and
internal refactors do not normally need one.

1. [Open a GitHub issue](https://github.com/Cloth-Foundation/cCloth/issues/new)
   titled `RFC: <short name>`.
2. State the problem and concrete use cases.
3. Specify the proposed syntax and semantics with valid and invalid examples.
4. Describe compiler, runtime, portability, and compatibility consequences.
5. Compare reasonable alternatives and list unresolved questions.
6. Define how the behavior will be tested and documented.

Keep the RFC scoped to one coherent decision. Implementation should begin
after the intended behavior is clear enough for review. The merged language
and architecture documents are the source of truth; the issue preserves the
discussion and alternatives.

## Implement a feature

Carry a feature through every compiler boundary it affects:

1. Update the language contract and grammar. Use the RFC process when the
   behavior is externally visible.
2. Add lexical, parser, and AST support where syntax changes.
3. Bind and type-check the behavior, then represent it explicitly in HIR.
4. Lower and verify any control-flow or data behavior in MIR.
5. Update target ABI, LLVM lowering, and runtime behavior when representation
   or execution changes.
6. Add focused unit tests, invalid-program diagnostics, and an end-to-end
   native test when the feature is executable.
7. Update the relevant design document and remove or amend any entry in
   [TODO.md](TODO.md).

Not every change touches every layer, but a pull request should make deliberate
boundaries explicit rather than stopping once new syntax parses.

Before requesting review:

```sh
cmake --build --preset dev --target check_format
cmake --build --preset dev
ctest --preset dev

cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
```

The `check_format` target is available when `clang-format` is installed.
C++ changes must follow [CODE_STYLE.md](CODE_STYLE.md), the repository's
Google C++ style variant for C++23.

## Pull, branch, and push

Fork the repository on GitHub, then clone your fork and register the canonical
repository as `upstream`:

```sh
git clone https://github.com/<your-account>/cCloth.git
cd cCloth
git remote add upstream https://github.com/Cloth-Foundation/cCloth.git
```

Start work from an up-to-date `master` branch:

```sh
git switch master
git pull --ff-only upstream master
git switch -c feature/<short-name>
```

Keep commits focused and use imperative commit subjects. Push the branch to
your fork:

```sh
git add <files>
git commit -m "Add <concise change>"
git push -u origin feature/<short-name>
```

Open a pull request against `Cloth-Foundation/cCloth:master`. Explain the
user-visible contract, affected compiler layers, tests, and any intentionally
deferred work. To refresh a branch while it is under review:

```sh
git fetch upstream
git rebase upstream/master
git push --force-with-lease
```

Do not commit build directories, generated executables, or unrelated
formatting changes.

## Repository map

| Path | Purpose |
| --- | --- |
| `include/cloth/` | Public compiler interfaces |
| `src/` | Compiler implementation and the `clothc` driver |
| `runtime/` | Native runtime and managed-heap support |
| `tests/` | Unit, diagnostic, project, backend, and native tests |
| `examples/` | Small Cloth programs |
| `docs/` | Language and compiler contracts |
| `cmake/` | Shared build, tooling, and test configuration |
| `TODO.md` | Deferred work and design guardrails |

Start with [Language design](docs/language_design.md) and the
[implemented grammar](docs/grammar.md). The main compiler boundaries are
documented in [semantic analysis](docs/semantic_analysis.md),
[control flow and MIR](docs/control_flow_and_mir.md),
[data layout and ABI](docs/data_layout_and_abi.md), the
[LLVM backend](docs/llvm_backend.md), and the
[native runtime](docs/native_runtime.md).
