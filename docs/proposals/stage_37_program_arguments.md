# Proposal: Stage 37 portable program arguments

Status: **complete — 37.4 exit audit passed 2026-09-06**.

This proposal defines one portable boundary for delivering process arguments to
Cloth applications. It extends the existing native entry contract without
adding a command-line parser, console API, operating-system string type, or
standard-library wrapper.

See the [compiler roadmap](../../ROADMAP.md#stage-37-portable-program-arguments)
and [work ledger](../../TODO.md#stage-37-portable-program-arguments).

## 1. Source contract

An executable may use any one of these entry shapes:

```cloth
static func Main() {}
static func Main(): void {}
static func Main(): int32 { return 0; }

static func Main(string[] args) {}
static func Main(string[] args): void {}
static func Main(string[] args): int32 { return 0; }
```

The parameter name is ordinary and may differ from `args`. Its type must be
exactly non-null `string[]`, whose elements are also non-null. Nullable arrays,
nullable elements, `object[]`, other array types, and additional parameters are
not eligible entry signatures.

Existing visibility, `static`, return, and typed-error rules remain unchanged.
An eligible form may declare `throws`; successful `void` completion returns
process status zero, successful `int32` completion returns that value, and an
uncaught Cloth error follows the existing reporter path.

Visibility follows Cloth capitalization. `Main` is therefore public by
spelling; lowercase `main` is a distinct private identifier and is never an
entry candidate.

Entry selection remains exact. A native program must contain one eligible
`Main`. More than one eligible form is ambiguous, including a zero-parameter
form beside an argument-taking form. A non-eligible function named `Main` does
not become an entry point.

## 2. Argument values

The array contains only application arguments. It excludes the executable name
or path supplied by the host. Argument count, order, empty arguments, leading
dashes, whitespace, and Unicode scalar values are preserved. Shell expansion
and quoting occur before Cloth receives the values and are not language
semantics.

Every argument becomes an immutable Cloth `string` whose UTF-8 backing storage
is owned by the runtime and remains valid throughout the invocation. Allocation
identity is not observable. No normalization, case conversion, locale
conversion, or replacement character is applied. Process argument mechanisms
cannot represent an embedded null code point, so Cloth does not invent one.

Host text conversion is strict:

- Windows input is decoded from the operating system's wide command line and
  unpaired UTF-16 surrogates are rejected.
- POSIX input bytes must be well-formed UTF-8.

Malformed host text terminates before user code with
`cloth runtime error: program argument is not valid Unicode`. It is not exposed
as a typed Cloth error because no Cloth callable has started.

The resulting array length must fit `int32`, matching every Cloth array. Count,
size, UTF decoding, allocation-size, and pointer-shape calculations are checked.
Allocation exhaustion and structurally invalid host inputs retain deterministic
runtime-failure behavior. Stage 37 adds no smaller policy limit than the host
and existing Cloth representation limits.

## 3. Runtime and GC ownership

The native entry adapter asks the runtime to construct the complete managed
`string[]` before calling `Main`. The runtime owns copied UTF-8 payloads; it does
not borrow `argv`, temporary conversion buffers, or Shuttle memory.

Construction must keep the partially built array and every completed string
reachable across all allocation safepoints. The adapter then roots the argument
array for the complete `Main` invocation. Array tracing keeps its string
elements alive. Normal collection may reclaim the graph after `Main` returns.

The runtime exposes one checked logical operation:

```text
cloth_rt_program_arguments(host_count, host_values) -> string[]
```

Its platform-specific implementation owns host decoding. Generated code treats
the returned array and string layouts exactly like source-created values. The
whole-project backend and separate-package linker must use the same entry
adapter contract.

## 4. Shuttle boundary

Shuttle accepts program arguments only for `run`, after an explicit `--`:

```sh
shuttle run -- first "two words" ""
```

`check` and `build` do not accept trailing program arguments. Shuttle stores and
forwards host-native argument values without interpreting UTF, parsing flags,
normalizing paths, expanding variables, or joining values into a command
string. An argument after `--` that resembles a Shuttle option belongs to the
Cloth program.

`shuttle run --` is equivalent to running with zero application arguments.
Build progress remains on standard error and application streams remain
unchanged. Spawn failures and program exit status retain their current Shuttle
contracts.

## 5. Packages and compatibility

The parameter uses existing `string` and array type identities, HIR/MIR forms,
call ABI rules, mangling, and artifact declaration records. Source-free entry
selection reads the serialized callable signature and never reopens source.
No standard-library declaration or implicit dependency is involved.

Stage 37 keeps artifact format **5**, compiler ABI **5**, process protocol **2**,
receipt schema **1**, manifest schema **1**, and toolchain-metadata schema **1**.
The new exported runtime operation advances runtime ABI **4 to 5** in 37.2.
All generated artifacts must carry runtime ABI 5. Existing compiler-identity
checks invalidate prior Shuttle state; capabilities and receipts do not gain a
runtime-ABI field.

The compiler-paired `cloth` package remains version `0.2.0`; Stage 37 changes no
standard-library source or public API. Shuttle's manifest and package versions
also remain unchanged.

## 6. Diagnostics

When functions named `Main` exist but none has an eligible signature, the
compiler reports that an entry must be public and static, take no parameters or
one non-null `string[]` parameter, and return no value or `int32`. Multiple
eligible entries retain the existing deterministic ambiguity diagnostic.

Malformed HIR, MIR, ABI, or artifact entry signatures are rejected before LLVM
emission or linking. A failed rebuild or link must not replace a completed
artifact or executable, and `shuttle run` must never launch stale output.

## 7. Verification

Implementation verification must cover:

- all six entry shapes, omitted and explicit `void`, `int32`, and `throws`;
- nullable, wrong-element, wrong-array, extra-parameter, private, instance,
  absent, and ambiguous entry failures;
- zero, one, many, empty, whitespace, option-like, and Unicode arguments with
  exact count and order;
- strict UTF-8 and UTF-16 rejection without user-code execution;
- forced collection during construction and throughout `Main`;
- whole-project, separate-package, and source-free entry equivalence;
- direct native execution and exact `shuttle run --` forwarding;
- program statuses, thrown errors, spawn failures, failure preservation, and
  stale-run prevention;
- verified LLVM before and after optimization on x86-64 and wasm32, plus native
  x86-64 execution; and
- development, sanitizer, Rust, editor, documentation, formatting, and
  repository gates.

## 8. Stage plan

1. **37.1 — Contract (complete).** Freeze source shapes, value and encoding
   semantics, entry selection, runtime/GC ownership, Shuttle forwarding,
   compatibility, diagnostics, verification, and non-goals.
2. **37.2 — Compiler and runtime (complete).** Accept the argument-taking entry
   signature, construct rooted managed values through runtime ABI 5, update
   direct and package entry adapters, and reject malformed internal state.
3. **37.3 — Shuttle integration (complete).** Add the explicit `run --`
   boundary, preserve host-native arguments and streams, and prove
   direct/Shuttle and whole/source-free equivalence.
4. **37.4 — Exit audit (complete).** Close encoding, resource, GC, compatibility,
   determinism, failure-preservation, native/cross-target, documentation, and
   repository matrices.

The 37.4 audit completed every verification category above without changing
the approved source or runtime contract. **Stage 37 is complete.**

## 9. Non-goals

Stage 37 does not add:

- console or file input, environment variables, working-directory access, or
  raw operating-system argument bytes;
- string-to-primitive parsing, flag parsing, option schemas, help generation,
  shell expansion, globbing, or command reconstruction;
- a standard-library `Args`, `Process`, `Console`, or application class;
- variadic functions, default parameters, general entry overloading, multiple
  entry names, or instance entry points;
- nullable argument arrays or elements, read-only arrays, slices, collections,
  iterators, or generics;
- WebAssembly command interfaces, WASI execution, a wasm runtime/linker, or
  native targets beyond the existing x86-64 path; or
- recovery from malformed host text or allocation failure after user code has
  begun.
