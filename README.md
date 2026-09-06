# Cloth

![GitHub Actions Workflow Status](https://img.shields.io/github/actions/workflow/status/Cloth-Foundation/cCloth/gradle.yml?style=for-the-badge)
![GitHub License](https://img.shields.io/github/license/Cloth-Foundation/cCloth?style=for-the-badge)
![GitHub Repo stars](https://img.shields.io/github/stars/Cloth-Foundation/cCloth?style=for-the-badge)
![GitHub forks](https://img.shields.io/github/forks/Cloth-Foundation/cCloth?style=for-the-badge)
![GitHub contributors](https://img.shields.io/github/contributors/Cloth-Foundation/cCloth?style=for-the-badge)

Cloth is a statically typed, LLVM-backed programming language for building
portable software with native compilation and managed memory. It brings
together the parts we value in C++, Java, C#, and Go while deliberately
reducing boilerplate, ceremony, and accidental complexity.

- From **C++**: ahead-of-time compilation, strong static types, and a
  performance-oriented toolchain—without requiring application code to manage
  object lifetimes manually.
- From **Java**: managed objects, familiar class-oriented organization, and a
  portable application model—without repeating the same structure and
  modifiers throughout a source file.
- From **C#**: an expressive object model and explicit nullability—within a
  smaller language surface designed around portable native output.
- From **Go**: simple visibility rules based on capitalization instead of
  repeated `public` and `private` declarations.

The goal is straightforward: write clear code once, compile it across targets,
and let the language and runtime handle memory safety and repetitive structure.
Cloth is its own language rather than a dialect or compatibility layer for any
of its influences.

```cloth
// HelloWorld.co
static func Main() {
  println("Hello, World!");
}
```

## Design philosophy

Cloth favors rules that remove repetition without hiding program behavior:

- Every `.co` file is an implicit class named after the file. `User.co`
  defines `User`; fields, functions, constructors, and nested types belong to
  it without an enclosing `class User { ... }` declaration.
- Names beginning with an uppercase ASCII letter are public. Lowercase and
  underscore-prefixed names are private.
- Objects and arrays use managed references backed by precise garbage
  collection. Ordinary Cloth code does not call `delete`, maintain reference
  counts, or annotate ownership.
- References are non-null by default. Nullable types use `T?`, with `?.`,
  `??`, and `!` making null behavior visible at the point of use.
- Packages follow the source tree, and imports use identifier paths rather
  than file-name strings.
- Compiler stages use explicit, verified representations before LLVM lowering,
  keeping language semantics independent of a particular machine target.

For example, `User.co` defines the `User` type directly. `name` is private and
`Greeting` is public because capitalization is part of the language contract:

```cloth
// User.co
string name;

User(string name) {
  self.name = name;
}

func Greeting(): string {
  return "Hello, " + name;
}
```

Cloth does not currently impose checked-exception declarations or routine
`try`/`catch` ceremony. A recoverable exception model remains deliberately
deferred until it has a small, coherent contract across the language, runtime,
and foreign-function boundary.

## Current status

Cloth is under active development. The language, compiler interfaces, runtime
ABI, and tooling are not stable. The compiler-paired standard library provides
`cloth.math::Math` plus a recursive `cloth.lang` prelude currently containing
`ArgumentError` and `StateError`. It is intentionally incomplete, and there is
not yet an external package registry.

The compiler currently provides a deterministic lexer, two-pass parser,
semantic analysis, typed HIR, control-flow MIR, a verified target ABI, and an
LLVM IR backend. The implemented language includes path-derived packages and
imports, functions and constructors, structured control flow, arrays, strings,
explicit nullability, managed objects, single inheritance, abstract and sealed
types, interfaces, [named value enums](docs/enums.md),
[value structs](docs/structs.md), overriding, class and interface dispatch,
covariant managed-reference returns, capitalization-based constructor
visibility, and `super` calls. Structured control flow includes `switch`, array
iteration, and classical `for` loops, with arithmetic compound assignment and
prefix/postfix numeric updates. Numeric values use contextual and explicitly
typed literals, lossless implicit widening, checked explicit conversions, and
`wrap`/`sat` integer conversion modes. Fixed-width integers also provide
bitwise and checked shift operators plus explicit little-endian and big-endian
`byte[]` meta operations. Typed errors provide declared `throws` contracts and
automatic propagation without `try`/`catch` syntax.

The backend emits LLVM IR for x86-64 and wasm32 layouts. Native executable
generation currently targets x86-64 and uses LLVM `llc` plus the configured C++
linker driver. [Shuttle](shuttle/README.md) is the separate official project,
build, and package manager. It supports validated local dependency graphs and
the versioned compiler process protocol. A complete standard library, remote
package registry, and broader native target support remain future work.

## Toolchain boundary

`clothc` owns the Cloth language: parsing, semantic analysis, verified compiler
representations, ABI rules, code generation, and language diagnostics. Shuttle
owns `Shuttle.toml`, workspaces, dependencies, build planning, compiler
invocation, caches, and user-facing build workflows.

The compiler does not parse Shuttle manifests, and Shuttle does not reproduce
language or ABI rules. They communicate through an explicit, versioned build
request. See [Shuttle and the Cloth compiler](docs/shuttle_and_compiler.md) for
the ownership and migration contract.

## Build from source

Requirements:

- CMake 3.25 or newer
- Ninja for the checked-in CMake presets
- A C++23 compiler such as recent Clang, GCC, or MSVC
- LLVM `llc` and a C++ linker driver to build native Cloth executables
- Optional LLVM `opt` for backend verification tests
- Rust 1.85 or newer with Cargo for Shuttle and the cross-tool tests

No third-party C++ libraries are required. From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
cargo build --manifest-path shuttle/Cargo.toml --locked
```

The development preset creates `build/dev`, enables tests, and treats compiler
warnings as errors. It also runs Shuttle's shared tests against that compiler.
For a compiler-only checkout, configure with `-DCLOTH_TEST_SHUTTLE=OFF`.
See [Testing](docs/testing.md) for sanitizer, coverage, and fuzzing presets.

## Build and run a Cloth project

The checked-in Hello World project demonstrates the supported Shuttle path.
From the repository root:

```sh
cargo run --manifest-path shuttle/Cargo.toml --locked -- run \
  --manifest-path examples/Shuttle.toml \
  --compiler build/dev/clothc
```

On Windows, use `build/dev/clothc.exe`. `shuttle check` validates and
type-checks the complete local package graph without emitting a program, while
`shuttle build` writes the native executable beneath the package's
`target/x86_64/` directory.

## Use the compiler directly

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
       [--source-root=<path>]
       [--check | --emit-llvm[=<path>] | --build=<path>]
       <source.co>...
```

Native `--build` output currently requires `--target=x86_64`.
`--check` validates the frontend and prints typed HIR without generating code
or artifacts. It supports the same source discovery and import rules.

## Projects and imports

Shuttle projects use the versioned `Shuttle.toml` manifest:

```text
my_app/
  Shuttle.toml
  src/
    Main.co
    models/
      User.co
```

Use Shuttle for dependency resolution and ordinary project builds:

```sh
shuttle run --manifest-path my_app/Shuttle.toml --compiler ./clothc
```

For compiler development, direct mode accepts an explicit source root and
never opens a manifest:

```sh
./clothc --source-root=my_app/src --build=build/my_app my_app/src/Main.co
```

Imports use identifier paths rather than file-name strings:

```cloth
import models::User;
import services.api.*;
import legacy::User as LegacyUser;
```

See [Packages and imports](docs/packages_and_imports.md) for path identity,
visibility, discovery, and collision rules.

Dependency aliases remain lowercase source-visible namespace prefixes. Package
names and manifest paths do not enter Cloth source syntax.

## Contributing

Cloth is production-minded even while its design is evolving. Changes should
keep contracts concise, diagnostics actionable, compiler boundaries explicit,
and tests proportional to the behavior being introduced.

### Follow the staged roadmap

[ROADMAP.md](ROADMAP.md) is the authoritative implementation order, and
[TODO.md](TODO.md) contains the concrete scheduled work and unscheduled
backlog. Only one stage may be active. Before feature implementation begins,
the stage must have approved scope, non-goals, exit criteria, work items, and an
explicit implementation go-ahead.

Design approval does not activate a stage. New ideas discovered during active
work belong in the unscheduled backlog unless they are required by an existing
exit criterion and the roadmap is updated first. This keeps a focused change
from silently becoming a broader language project.

### Propose a change with an RFC

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

Keep an RFC scoped to one coherent decision. Implementation should begin once
the intended behavior is clear enough for review, the work is assigned to the
active roadmap stage, and implementation is explicitly approved. Merged
language and architecture documents are the source of truth; the issue
preserves discussion and alternatives.

### Implement a feature

Carry a feature through every compiler boundary it affects:

1. Update the language contract and grammar. Use the RFC process when behavior
   is externally visible.
2. Add lexical, parser, and AST support where syntax changes.
3. Bind and type-check the behavior, then represent it explicitly in HIR.
4. Lower and verify control-flow or data behavior in MIR.
5. Update the target ABI, LLVM lowering, and runtime when representation or
   execution changes.
6. Add focused unit tests, invalid-program diagnostics, and an end-to-end
   native test when the feature is executable.
7. Update the relevant design document and remove or amend any entry in
   [TODO.md](TODO.md).

Not every change touches every layer, but a pull request should identify its
deliberate boundaries instead of stopping once new syntax parses.

Before requesting review:

```sh
cmake --build --preset dev --target check_format
cmake --build --preset dev
ctest --preset dev

cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
```

The `check_format` target is available when `clang-format` is installed. C++
changes must follow [CODE_STYLE.md](CODE_STYLE.md), the repository's Google C++
style variant for C++23.

### Pull, branch, and push

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
| `tests/unit/` | Compiler and runtime unit suites |
| `tests/integration/` | CLI, native, diagnostic, and multi-file test data |
| `tests/fuzz/` | Fuzz targets and curated seed corpora |
| `examples/` | Small Cloth programs |
| `editors/` | Editor integrations, including the VS Code extension |
| `docs/` | Language and compiler contracts |
| `cmake/` | Shared build, tooling, and test configuration |
| `ROADMAP.md` | Authoritative stage order, boundaries, and exit criteria |
| `TODO.md` | Scheduled stage work and unscheduled backlog |

Start with the [compiler roadmap](ROADMAP.md),
[Language design](docs/language_design.md), and the
[implemented grammar](docs/grammar.md). The main compiler boundaries are
documented in [semantic analysis](docs/semantic_analysis.md),
[numeric literals and widening](docs/numeric_conversions.md),
[integer binary data](docs/integer_binary_data.md),
[interfaces](docs/interfaces.md),
[control flow and MIR](docs/control_flow_and_mir.md),
[data layout and ABI](docs/data_layout_and_abi.md), the
[LLVM backend](docs/llvm_backend.md), and the
[native runtime](docs/native_runtime.md).
