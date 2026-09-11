# Stage 48: Self-hosted testing and Bazel orchestration

Status: **complete as of 2026-09-10**.

Stage 48 replaces the self-hosted compiler's monolithic test executable with
independent Bazel test targets backed by a testing library written in Cloth.
Bazel owns repository test scheduling, declared inputs, caching, isolation, and
reporting. Shuttle remains Cloth's user-facing project, build, and package
system.

See the [compiler roadmap](../../ROADMAP.md), [work ledger](../../TODO.md),
[Shuttle boundary](../shuttle_and_compiler.md), and
[self-hosted compiler architecture].

## 1. Authority and scope

Stage 48 applies to the self-hosted compiler repository at `F:\Cloth`. It does
not reopen the frozen C++23 bootstrap compiler. The existing C++ compiler
continues only as:

- the bootstrap executable that compiles the self-hosted compiler;
- the observable-behavior oracle for boundaries whose authority has not moved;
  and
- the subject of its existing frozen regression and sanitizer coverage.

New testing framework code, Bazel rules, test organization, and test cases
belong to the self-hosted repository. A parity failure may justify a narrowly
scoped bootstrap correctness repair, but not a new C++ language feature or a
second implementation path.

Completing Stage 48 makes Bazel authoritative for tests owned by `F:\Cloth`.
It does not transfer lexer, parser, semantic, lowering, artifact, or runtime
authority. Each compiler subsystem still requires its separately approved
parity and authority-transfer checkpoint.

## 2. Bazel and Shuttle boundary

Shuttle remains the official user-facing system for:

- `Shuttle.toml` and future lockfiles;
- workspace and package discovery;
- dependency selection and aliases;
- normal user build, check, run, test, and publishing workflows; and
- local build-state and output-directory policy.

Bazel is an internal compiler-repository build and test orchestrator. Bazel
BUILD labels describe only repository validation targets. They do not become
Cloth package coordinates, source imports, or a second public manifest format.

The Bazel rules invoke `clothc` directly through compiler process protocol 2.
They must not start Shuttle inside a Bazel compilation action. Nesting one
scheduler and cache inside the other would make inputs, ownership, progress,
and failure behavior ambiguous. A separate compatibility test may invoke
Shuttle as the program under test.

The rules use the existing protocol rather than private compiler data, C++
headers, an FFI, or implementation-specific output inspection. The compiler
continues to own source semantics, artifacts, receipts, diagnostics, linking,
and atomic publication.

## 3. Repository foundation

The self-hosted repository owns:

```text
.bazelversion
.bazelrc
MODULE.bazel
MODULE.bazel.lock
tools/
  bazel/
    cloth/
      BUILD.bazel
      defs.bzl
      extensions.bzl
      README.md
      private/
        BUILD.bazel
        extensions.bzl
        protocol_driver.py
        providers.bzl
        rules.bzl
        toolchain.bzl
```

Stage 48.1 pins Bazel 9.2.0, declares the `cloth_compiler` Bzlmod root module,
records Bazel output symlinks as generated files, and declares
`//tools/bazel/cloth:toolchain_type`. The root module version remains `0.0.0`
because this repository is not a published Bazel rules module.

`.bazelrc` may set repository-wide deterministic diagnostics and test-output
policy. It cannot contain an absolute machine path, username, workspace-local
compiler location, credential, remote-cache address, or platform-specific
developer preference.

`tools/bazel/cloth` owns Cloth-specific Starlark. Public symbols are re-exported
from `defs.bzl`. Implementation files live under `private/`; ordinary compiler
BUILD files load only `defs.bzl`, while the integration package composes the
toolchain implementation. Empty future files and placeholder rule families are
not added.

Starlark uses four-space indentation and buildifier-compatible formatting.
Names use `lower_snake_case`. Rule and provider documentation states the
declared inputs, outputs, toolchains, and execution behavior.

## 4. Bootstrap toolchain

Stage 48.2 implements one mandatory Cloth toolchain type. Its provider
contains:

- the executable compiler artifact;
- the compiler-paired `cloth-toolchain.json` descriptor;
- the exact standard-library source closure selected by that descriptor;
- the repository-owned protocol adapter and its declared Python runtime;
- the bootstrap compiler's declared Windows host-runtime DLL closure;
- the host execution properties required by the adapter;
- the supported Cloth target and artifact kinds; and
- the required process, receipt, artifact, compiler ABI, and runtime ABI
  versions.

The initial registered toolchain imports the frozen C++ bootstrap distribution.
It is supplied through an explicit repository configuration, not discovered
through `PATH`, the current working directory, a registry, or a parent checkout
guess. The imported executable, descriptor, and standard-library files are
declared Bazel inputs and therefore participate in action identity.

Checkpoint 48.2 registers Windows host execution explicitly. Linux and macOS
host registrations must preserve the same provider and rule interface and are
accepted only with the Stage 48.4 portability and hermeticity audit.

Missing, unreadable, mismatched, or incompatible toolchain files fail during
repository analysis with one actionable diagnostic. Rules do not silently
fall back to another compiler.

Once the self-hosted compiler can reproducibly compile and link itself, a later
authority checkpoint registers a self-hosted implementation of the same
toolchain type. `cloth_library`, `cloth_binary`, and `cloth_test` targets do not
change when that implementation changes.

## 5. Cloth build rules

Stage 48.2 adds three repository-private rules:

- `cloth_library` compiles one explicitly described Cloth package into one
  compiler-owned package artifact and publishes a `ClothPackageInfo` provider.
- `cloth_binary` compiles its root package and links one executable from the
  exact transitive object-artifact closure.
- `cloth_test` builds an isolated test executable and exposes it to Bazel as a
  native test target with declared runfiles.

`ClothPackageInfo` carries logical package name and version, artifact kind,
target, artifact and receipt outputs, direct alias edges, and the deterministic
transitive artifact closure. It does not expose compiler-private declarations,
layouts, symbols, or native object paths.

Every action uses `ctx.actions.run` with an argument vector. It does not
construct a shell command. Package and dependency records use the canonical
protocol-2 order. Sources, dependency artifacts, toolchain files, data, and
outputs are declared before execution. Actions write only beneath their Bazel
output directory.

Compilation cannot recursively enumerate undeclared workspace source. A rule
must stage its declared `.co` inputs into an action-owned source tree that
preserves their package-relative paths before invoking `clothc`. Duplicate,
escaping, case-colliding, or non-`.co` source mappings fail during analysis.

The rules do not parse Cloth source, infer imports, read `Shuttle.toml`, resolve
versions, or reproduce compiler semantics. BUILD dependencies and explicit
aliases define the repository test graph.

## 6. Cloth test support

Stage 48.2 adds a repository-private package written in Cloth and exported
to tests under the dependency alias `testing`. Initial tests import
`testing::Assert`; they do not claim the reserved `cloth` standard-library
namespace.

The initial assertion surface is:

- `Assert.True` and `Assert.False`;
- `Assert.Equal` and `Assert.NotEqual` for supported scalar, string, enum, and
  object identities;
- `Assert.Null` and `Assert.NotNull`;
- `Assert.Fail`; and
- `TestFailure`, a typed error carrying the assertion context and stable
  diagnostic text.

Promotion to a public `cloth.test` standard-library package is a separate
standard-library API decision. Stage 48 does not silently turn compiler test
support into a permanent user-facing library.

Each `cloth_test` names one test class with one public static `Run` function.
The rule generates the entry source that invokes that function and translates
normal completion or an unhandled `TestFailure` into Bazel's process result.
This avoids a mutable global registry, reflection, source scanning, and
boilerplate `Main` functions.

The generated entry declares `throws Error`, so a test may propagate the typed
I/O, state, and assertion failures it actually exercises without duplicating
that list in BUILD metadata. Test code still declares its precise `Run` effects.

Stage 48 adds no `test` keyword, attribute, annotation, macro, reflection
feature, function value, or special compiler mode. Test classes and assertions
are ordinary Cloth code.

## 7. Execution and result contract

One Bazel test target is one isolated process:

- status 0 means pass;
- any nonzero status means failure;
- assertion diagnostics go to standard error;
- successful tests are silent unless the test intentionally publishes a
  record; and
- output ordering is deterministic.

An assertion diagnostic identifies the test target, assertion operation,
caller-supplied context, expected value when applicable, and actual value when
applicable. It cannot depend on addresses, object allocation order, wall-clock
time, locale, terminal width, or color.

Test data is declared through the rule's `data` attribute and resolved from
Bazel runfiles. Tests cannot assume that the process working directory is the
repository root. Temporary output is confined to Bazel's supplied test
temporary directory and is not a source input.

The checked-in configuration enables Bazel's native runfiles tree. Tests open
declared data by repository-relative path from the runfiles workspace, which is
the same contract on Windows, Linux, and macOS and does not expose the checkout.

The generated runner and runfiles adapter are rule-owned implementation
details. They must support Windows first and preserve an explicit path contract
that can be implemented on Linux and macOS without changing test source.

## 8. Suites and responsiveness

The repository exposes explicit suites instead of treating every check as one
opaque program:

- `//tests:presubmit` contains deterministic unit and bounded integration tests
  intended for the normal edit loop.
- `//tests:full` includes presubmit plus parity, GC, depth, scale, resource, and
  Shuttle compatibility tests.

Tests carry one primary category tag: `unit`, `integration`, `parity`, `gc`, or
`resource`. Bazel `size` and `timeout` values reflect measured behavior.
Resource and exhaustive parity tests remain individually addressable; they are
not hidden inside an ordinary unit target.

The default test configuration prints progress, failed-test output, and a
detailed summary. It does not print successful test bodies by default.
Developers can run a single target or package without rebuilding and executing
unrelated parser, lexer, GC, or parity cases.

No numerical timing promise is frozen before the first measured migration.
Stage 48.4 records cold and warm timings and treats an unexpected regression as
an exit failure.

## 9. Migration and retirement

Stage 48.3 converts the current `tests/self_host/src/BootstrapMain.co` routing
ladder into independently named tests. Existing check implementations may be
adapted in place when they own one coherent invariant. Status-code ladders are
replaced with assertions that retain useful failure context.

Parity record writers remain `cloth_binary` tools consumed by differential
tests. They are not assertion tests. GC, depth, failure-injection, and resource
checks remain distinct targets with explicit tags and timeouts.

During migration, the existing Shuttle test executable and Bazel targets run
against the same fixtures. The old executable is removed only after target-by-
target result equivalence is demonstrated. One focused Shuttle compatibility
test remains because Shuttle is still a supported product boundary.

The CMake `cloth_self_host_frontend` bridge is narrowed or removed only after
Bazel owns equivalent self-hosted checks and the cross-compiler parity driver
has a declared bootstrap-oracle input. Existing C++ unit, integration,
sanitizer, and compiler-oracle tests are not migrated into `F:\Cloth`.

## 10. Security and determinism

Bazel actions receive no network access as part of the Stage 48 contract.
Source tests cannot download dependencies, inspect unrelated workspace files,
read credentials, or mutate source directories.

External compiler, standard-library, action-interpreter, and Windows compiler-
runtime locations enter only through the bootstrap repository configuration.
Configuration values are not embedded in artifacts, receipts, test diagnostics,
or checked-in BUILD files. Imported files are validated and become declared
action or runfiles inputs.

Remote execution and shared remote caching remain disabled until path
independence, toolchain closure, platform constraints, and reproducible native
linking are verified. A local cache hit must reproduce the same artifact and
test result as a clean action.

Failures preserve completed outputs according to the compiler protocol. Bazel
must not accept a stale artifact, missing receipt, truncated receipt, partial
test output, crashed process, or timeout as success.

## 11. Checkpoint plan

1. **48.1 — Contract and repository foundation (complete).** Freeze authority,
   Bazel/Shuttle ownership, bootstrap toolchain inputs, rule and test-result
   contracts, source staging, suite taxonomy, migration, security,
   compatibility, and non-goals. Pin Bazel, add the root Bzlmod configuration,
   declare the Cloth toolchain type, and ignore generated Bazel links.
2. **48.2 — Rules and Cloth test support (complete).** Implement the bootstrap
   toolchain repository, `ClothToolchainInfo`, `ClothPackageInfo`, `cloth_library`,
   `cloth_binary`, `cloth_test`, generated entry/runfiles support, assertions,
   typed failures, and focused rule analysis/execution coverage.
3. **48.3 — Self-hosted test migration (complete).** Split the monolithic test executable
   into isolated lexer, parser, syntax, parity, GC, failure, depth, and resource
   targets; establish presubmit/full suites; preserve fixtures and result
   equivalence; and retain one explicit Shuttle compatibility target.
4. **48.4 — Integration and authority audit (complete).** Verify declared-input
   hermeticity, clean/warm caching, deterministic artifacts, parallel execution,
   Windows paths, failed output, timeouts, both Cloth targets, cross-compiler
   parity, complete old/new coverage equivalence, documentation, and repository
   gates. Make Bazel authoritative for self-hosted compiler tests and retire
   superseded orchestration.

## 12. Compatibility

Stage 48 changes no Cloth source grammar, compiler artifact, compiler ABI,
runtime ABI, standard-library version, Shuttle manifest, compiler process
protocol, receipt schema, public compiler CLI, or editor grammar.
Compatibility remains artifact/compiler/runtime **8/7/11**, schemas
**2/1/1/1**, and compiler-paired `cloth` **v0.5.0**.

Bazel BUILD syntax and private Starlark providers are compiler-repository
interfaces. They are not language compatibility promises and are not published
as a general `rules_cloth` distribution during Stage 48.

## 13. Non-goals

Stage 48 does not add language syntax, reflection, function values, stack-trace
metadata, coverage instrumentation, fuzzing, benchmarks, snapshot updating,
test retries, flaky-test acceptance, remote execution, remote caching, package
registries, remote test dependencies, a public `cloth.test` package, general
`rules_cloth` distribution, C++ build migration, Shuttle replacement, compiler
feature work, or compiler authority transfer.

[self-hosted compiler architecture]: https://github.com/Cloth-Foundation/Cloth/blob/master/ARCHITECTURE.md
