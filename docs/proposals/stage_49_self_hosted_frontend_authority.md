# Stage 49: Self-hosted frontend integration and parser authority

Status: **complete — 49.4 authority audit passed 2026-09-10**.

Stage 49 turns the verified self-hosted lexer and two-pass parser into the
production package frontend. It introduces one deterministic package
coordinator, production diagnostics, and an explicit authority-transfer audit.
The C++ compiler remains the bootstrap implementation and comparison oracle;
it does not receive new language features.

See the [compiler roadmap](../../ROADMAP.md), [work ledger](../../TODO.md),
[Stage 47 definition parser](stage_47_self_hosted_definition_parser.md), and
[Stage 48 self-hosted testing](stage_48_self_hosted_testing.md).

## 1. Authority and scope

Stages 44, 46, and 47 proved the self-hosted lexer, declaration pass, and
definition pass as isolated compiler subsystems. Stage 49 connects those
subsystems at the package boundary used by a real compiler driver. It does not
add another parser, a test-only dispatcher, or a parallel syntax model.

The C++ frontend remained authoritative through 49.3. During that interval its
accepted syntax, structured diagnostics, recovery behavior, source ranges,
resource behavior, and deterministic ordering were the observable-behavior
oracle. The complete 49.4 audit found no unexplained divergence and transferred
package lexer and parser authority to the self-hosted frontend.

After the transfer, the self-hosted lexer and parser are authoritative for
frontend implementation behavior. Language proposals and user documentation
remain the normative source-language contracts. The frozen C++ frontend is
retained only as long as it is required to build the self-hosted compiler and
to provide deliberately preserved differential coverage.

## 2. Ownership boundaries

Shuttle owns workspace traversal, package discovery, dependency selection,
source-set construction, and the versioned compiler request. Bazel owns the
declared source closure of repository builds and tests. A direct compiler CLI
owns only the explicit paths supplied to that invocation.

The Stage 49 frontend consumes an explicit package source set. It never scans a
directory, follows an import to discover a file, opens an undeclared source,
loads a dependency, accesses the network, or writes into a source tree. File
loading remains at the driver or build-protocol boundary; parsing consumes
managed `SourceFile` values.

Import resolution, dependency graphs, symbols, and type identity begin after
the syntax boundary. The frontend retains imports as syntax and does not infer
additional package inputs from them.

## 3. Package input and source identity

`PackageFrontendInput` contains one owning `PackageIdentity` and an immutable
sequence of `PackageSource` values. A Shuttle-owned input uses the established
lowercase package-name grammar and a valid exact SemVer 2.0.0 version. A direct
standalone input has both owner fields empty. Mixed empty/non-empty owner fields
are invalid. Existing compiler-request validation remains responsible for the
reserved `cloth` identity. The version is retained for the later semantic
identity boundary but cannot alter lexing or parsing. The source sequence may
be empty; its canonical result is valid and contains no file results.

Each `PackageSource` contains:

- a normalized logical path relative to the package source root; and
- the loaded `SourceFile` whose diagnostic path identifies the user's file.

The logical path is the identity input. It is printable ASCII, uses `/`
separators, is relative, ends in `.co`, and contains no empty, `.` or `..`
component, backslash, drive prefix, control byte, or NUL byte. Physical
`SourceFile` diagnostic paths retain their independently accepted UTF-8. Every
logical directory component is a valid Cloth identifier. The first source-
package component cannot claim the reserved `cloth` root. The final file stem
is passed to the declaration parser so an invalid implicit class name remains
a source diagnostic rather than becoming a host-dependent path failure.

For `api/Data.co` in owning package `fib`, the coordinator derives:

```text
source package: api
file class:     Data
qualified name: fib.api.Data
```

A root file has an empty source package, so `Main.co` in `fib` becomes
`fib.Main`. The declaration outline and syntax tree record the owning package,
source package, and complete qualified name separately; they do not confuse
those values with the manifest version or physical checkout path. The owning
package name participates in compiler identity but does not become a prefix in
Cloth import syntax.

The coordinator rejects duplicate logical paths and paths that collide under
ASCII case folding before lexing. This makes the accepted source set identical
on case-sensitive and case-insensitive hosts. Package sources are processed in
ascending unsigned UTF-8 byte order of normalized logical path, independent of
request order, filesystem enumeration, locale, or host path syntax.

Malformed package input is a compiler-request error. Invalid Cloth text is a
source diagnostic. The boundary checks sizes and cumulative counts with checked
integer arithmetic before allocation or conversion; it never relies on
overflow, allocation failure, or a host exception to classify input.

## 4. File and package results

One `FileFrontendResult` owns the complete lifetime closure for one source:

- its `PackageSource` and derived source identity;
- its `LexerResult`;
- a nullable `DeclarationResult`;
- a nullable `DefinitionResult`; and
- aggregate validity.

Lexical errors stop parsing for that file, because parser tokens cannot be
trusted after a failed lexical result. They do not stop independent files in
the same package. A lexically valid file always runs both parser passes,
including their recovery paths. `DefinitionResult` already owns the combined
declaration and definition diagnostic sequence, so declaration diagnostics are
not appended a second time.

`PackageFrontendResult` owns the immutable, canonically ordered file results
and package validity. It exists only for an accepted input and is valid exactly
when every file result is valid. Syntax success is not semantic success: a
valid package result may still contain unresolved imports, names, types,
overloads, inheritance relationships, or effects.

Result objects retain all managed storage reachable from published tokens,
spans, outlines, and syntax handles. Callers do not keep hidden native pointers
or reconstruct a tree after publication. Future garbage collection may reclaim
an entire file result only after no package result or downstream phase retains
it.

## 5. Coordinator processing contract

`FrontendCoordinator.Check` validates and sorts the package input, then runs
the lexer, declaration parser, and definition parser once per file. The
coordinator may later process independent files concurrently, but observable
result and diagnostic order must remain the canonical order defined here.

A source failure is represented by structured diagnostics and a false result;
it is not thrown as an `IoError` or `StateError`. Invalid API construction,
forged phase ownership, and violated compiler invariants remain `StateError`.
File loading failures remain `IoError` at the caller boundary.

The coordinator performs syntax work only. It neither binds declarations
across files nor rejects two files for declaring the same semantic name. Those
checks require the Stage 50 symbol and type-identity model.

The existing public `clothc check <file.co>` form remains valid. In 49.3 the
self-hosted driver routes it through the package coordinator as a one-source
package and preserves success silence and process status. Stage 49 does not add
a temporary recursive-discovery CLI or bypass the compiler process protocol.

## 6. Diagnostic contract

Lexer and parser diagnostic kinds, primary ranges, and related ranges remain
structured until presentation. The package result merges no diagnostic text
into syntax nodes and does not discard a kind merely because another error is
nearby.

Presentation order is canonical package file order followed by the phase-owned
source order inside each file. Lexically invalid files contain lexer
diagnostics only. Parser diagnostics come from the definition result's already
combined sequence. Repeated runs and future parallel execution must produce
byte-identical diagnostic output.

Stage 49.3 adds exhaustive kind-to-message catalogs and one production
renderer. Its primary line format is:

```text
<path>:<line>:<column>: error: <message>
```

Related locations are emitted immediately after their primary diagnostic with
the matching stable note text. Diagnostics go to standard error; standard
output remains empty for `check`. Paths and arbitrary source bytes are escaped
without terminal control injection. Missing message mappings are compiler
errors and are covered exhaustively; a generic `syntax error` fallback is not
production behavior.

`DiagnosticSeverity` owns the semantic distinction between `error`, `warning`,
and `note`; Stage 49 emits errors and related notes. A later rich renderer may
add a separately selected output preset or explicit configuration for plain
text, styled terminals, and machine records. Presentation presets cannot
change a diagnostic's severity, ordering, source range, or process status.

Color, source excerpts, localization, machine-readable diagnostics, warning
policy, diagnostic recovery suggestions, and output-style presets require
later contracts.

## 7. Determinism, security, and resources

The frontend's output depends only on its declared package identity, version,
logical source paths, source bytes, and compiler compatibility constants. It
does not depend on current directory, absolute checkout location, timestamps,
locale, environment variables, filesystem case behavior, or input order.

All path validation and sorting operate on bytes with explicit ASCII rules.
Diagnostic rendering escapes control bytes. Counts, offsets, path lengths, and
cumulative source sizes use checked arithmetic compatible with the existing
managed sequence and `SourceFile` representations. Existing lexer and parser
depth, token, diagnostic, and constant-expression limits remain authoritative;
the package layer does not silently raise or bypass them.

One invalid file cannot suppress valid neighboring results, replace a completed
compiler artifact, or leave a partially published syntax tree. The coordinator
does not mutate a caller's input sequence. Repeated and relocated runs must
publish equivalent result records.

## 8. Implementation organization

Production package integration belongs under `src/frontend/package/` in
`F:\Cloth`.
The driver may translate CLI or protocol inputs into the frontend package
model, but it does not own lexing, parsing, diagnostic catalogs, or syntax-tree
assembly. Test adapters stay under `tests/` and consume the same production
coordinator.

The planned production files are organized by responsibility:

```text
src/frontend/package/
  PackageIdentity.co
  PackagePath.co
  PackageSource.co
  PackageFrontendInput.co
  FileFrontendResult.co
  PackageFrontendResult.co
  FrontendCoordinator.co
src/frontend/diagnostic/
  DiagnosticSeverity.co
  LexDiagnosticText.co
  ParseDiagnosticText.co
  DiagnosticRenderer.co
```

Names may change only when implementation exposes a clearer ownership split;
the input, result, lifetime, diagnostic, and authority contracts do not.

## 9. Verification and authority transfer

Stage 49 testing uses independent Bazel targets and the existing declared C++
oracles. It covers:

- root and nested logical paths, qualified identities, Windows diagnostic
  paths, shuffled requests, duplicates, case collisions, and invalid paths;
- empty, valid, lexically invalid, declaration-invalid, and definition-invalid
  files, including mixed-validity packages where later files still run;
- exact structured and rendered diagnostics, related locations, control-byte
  escaping, standard streams, and process status;
- all existing lexer, declaration, definition, recovery, depth, GC, resource,
  and production-source differential corpora;
- retained result graphs across forced collections and release after owners are
  dropped;
- repeated, parallel, relocated, x86-64, and wasm32 checks; and
- Bazel presubmit/full, Shuttle compatibility, formatting, documentation,
  link, and repository-hygiene gates.

Stage 49.4 records every measured corpus count and gate result. Authority does
not transfer on sampled syntax alone. Any unexplained accepted-language,
diagnostic, recovery, ordering, resource, or real-project divergence blocks the
checkpoint.

The completed audit covers 614 lexer inputs, 32 bounded and 188 production
declaration inputs, and 43 bounded and 188 production definition inputs with
byte-exact C++/Cloth records. All 82 uncached Bazel full-suite targets pass,
including package validation, malformed state, exact output, GC, depth,
relocated parallel Shuttle builds, x86-64 execution, wasm32 checking, and
failed-output preservation. Development and Clang ASan/UBSan unit suites each
pass all 85 entries; the sanitized compiler also checks the complete production
self-hosted source package for both targets. These results satisfy the transfer
gate without deleting the C++ bootstrap or its declared differential oracles.

## 10. Checkpoint plan

1. **49.1 — Frontend authority contract (complete).** Freeze ownership,
   package source identity, input and result models, phase behavior,
   diagnostics, determinism, security, authority transfer, compatibility,
   verification, and non-goals.
2. **49.2 — Package frontend coordinator (complete).** Implement package
   inputs, derived identities, file/package results, deterministic validation
   and ordering, production phase composition, lifetime verification, and
   focused Bazel coverage.
3. **49.3 — Production diagnostics and driver (complete).** Implement exhaustive
   diagnostic catalogs, safe deterministic rendering, related notes, standard-
   stream/status behavior, and route the self-hosted `check` driver through the
   production coordinator.
4. **49.4 — Integration and authority audit (complete).** Complete package, malformed,
   recovery, resource, GC, determinism, target, real-project, cross-compiler,
   build-system, documentation, and repository gates; then transfer parser
   authority to the self-hosted frontend.

## 11. Compatibility

Stage 49 adds compiler-internal managed types and changes which implementation
owns the frontend after its exit audit. The 49.3 standard-error foundation adds
`Console.WriteError` and `Console.WriteErrorLine`, advances runtime ABI to
**12**, and publishes compiler-paired `cloth` **v0.6.0**. It changes no Cloth
source grammar, artifact format, compiler ABI, Shuttle manifest or process
protocol, receipt schema, public import syntax, or editor grammar.
Compatibility is artifact/compiler/runtime **8/7/12** with schemas
**2/1/1/1**.

The exact self-hosted CLI diagnostic text becomes production behavior in 49.3,
but it is not a versioned machine protocol. Tools continue to consume the
existing structured compiler process response where applicable.

## 12. Non-goals

Stage 49 does not add source syntax, symbol tables, import resolution,
dependency loading, name or type checking, overload resolution, flow analysis,
constant evaluation, HIR, MIR, ABI work, LLVM lowering, code generation,
linking, a new compiler process protocol, directory discovery, an incremental
frontend cache, a language server, diagnostic color or excerpts, public
standard-library APIs beyond the standard-error foundation, C++ parser
deletion, native-target support, or remote execution. Those boundaries require
their own approved stages.
