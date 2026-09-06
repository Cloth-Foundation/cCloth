# Cloth testing and diagnostic builds

## Stage 36.4 standard-library prelude exit audit

Completed on Windows on 2026-09-06 with development and ASan/UBSan compilers.
Each configuration passes all 255 CTests, including 36 compiler-backed Shuttle
toolchain cases and 32 native cases. All 49 ordinary Rust tests, Rust 1.85,
warning-denied Clippy, Rust and C++ formatting, 12 compiler-backed editor checks
per compiler, local Markdown links, and repository whitespace checks pass.

The lookup matrix covers every public file-type kind at arbitrary depth beneath
`cloth.lang`, private capitalization, the `cloth.language` boundary, every
precedence level, exact and aliased imports, nonrecursive explicit wildcards,
global short-name and core collisions, absent library input, malformed
artifacts, and source-order-independent diagnostics. Whole-project and
source-free consumers emit verified raw and optimized LLVM for x86-64 and
wasm32. The standard library bootstraps without a self-edge, and independent
x86-64 objects link and execute the initial error API.

Shuttle verifies exact library selection, recursive prelude lookup through
artifacts, one-job/four-job relocation determinism, warm reuse, exact
invalidation, failed-output preservation, and stale-run prevention. The audit
increased only the scoped timeout for the multi-artifact standard-library CTest
to tolerate concurrent sanitizer instrumentation. Production compiler,
runtime, Shuttle, and library behavior did not change. Compatibility remains
artifact/compiler/runtime 5/5/4 and process/receipt/manifest/toolchain schemas
2/1/1/1. **Stage 36 is complete.**

## Stage 36.3 initial standard-library API

Completed on Windows on 2026-09-05 and amended 2026-09-06. The `cloth` package
remains `0.2.0` and provides the source-defined, extensible
`cloth.lang.errors.ArgumentError` and `cloth.lang.errors.StateError` types.
Both provide default and message constructors, inherit the compiler-owned
`Error.Message` contract, and resolve without an import from whole-project
source or verified package artifacts.

The integration matrix constructs both types, reads inherited messages, and
emits verified LLVM before and after optimization on x86-64 and wasm32. Nested
synthetic declarations cover every file kind, arbitrary prelude depth,
namespace boundaries, global short-name collisions, core collisions, and
deterministic diagnostics. The native package path links independent `cloth`
and application artifacts and prints both messages. An application error
derives from `ArgumentError` in a source-free consumer, and the MIR verifier
recognizes the ownerless compiler `Error.Message` field on every error-class
receiver.

All 253 development CTests, 36 compiler-backed Shuttle toolchain cases, 32
native Shuttle cases, and 49 ordinary Rust tests pass. Exact `cloth` version
and digest continue through capability, metadata, artifact, receipt, cache,
invalidation, and link paths. C++/Rust formatting and warning-denied Clippy
pass, as do all local Markdown targets and repository whitespace checks.
Compatibility remains artifact/compiler/runtime 5/5/4 and process/receipt/
manifest/toolchain schemas 2/1/1/1. The completed Stage 36.4 audit is recorded
above.

## Stage 36.2 standard-library prelude resolution

Completed on Windows on 2026-09-05. Semantic analysis now derives one
deterministically ordered type-name fallback from public file declarations
directly in the canonical `cloth.lang` package. The same path consumes
whole-project declarations and verified source-free package views. Existing
same-package, explicit, aliased, and wildcard bindings remain higher priority;
private files and child packages remain absent. Core-name conflicts are
reported at the rejected library declaration.

Focused compiler coverage exercises classes, errors, interfaces, enums, and
structs; lexical, member, same-package, exact, aliased, wildcard, prelude, and
core lookup boundaries; private and nested types; explicit wildcard ambiguity;
missing library input; malformed artifacts; stable diagnostics; and LLVM
emission for x86-64 and wasm32. The complete development configuration passes
all 253 CTests.

The public Shuttle suite adds a temporary paired-library fixture whose
synthetic `cloth.lang` type is compiled without a self-dependency and consumed
only through its artifact. One-job and four-job package artifacts are byte-
identical on x86-64 and wasm32. All 36 compiler-backed toolchain checks and 49
ordinary Rust tests pass, together with warning-denied Clippy and Rust/C++
formatting. Shuttle production code, production standard-library source,
package version `0.1.0`, and compatibility versions 5/5/4 and 2/1/1/1 remain
unchanged.

## Stage 36.1 standard-library prelude contract

Approved and recorded on Windows on 2026-09-05. The contract defines
`cloth.lang` as a nonrecursive, type-only fallback after current-package types,
explicit imports and aliases, and explicit wildcards but before compiler-owned
core symbols. It freezes public/private eligibility, deterministic ordering,
whole-project and artifact ownership, version evolution, diagnostics,
verification, and non-goals.

This checkpoint changes roadmap, work-ledger, and contract documentation only.
It adds no synthetic AST import, standard-library declaration, compiler or
Shuttle production behavior, user-facing claim, compatibility transition, or
library version change. Local Markdown targets and repository whitespace gates
passed. The separately authorized 36.2 implementation is recorded above.

## Stage 35.4 standard-library foundation exit audit

Verified on Windows on 2026-09-05 with development and ASan/UBSan compilers.
Both configurations pass all 255 CTests, including 35 public Shuttle toolchain
cases and 32 native cases. All 49 ordinary Rust tests, Rust 1.85, warning-denied
Clippy, Rust and C++ formatting, 12 compiler-backed editor checks per compiler,
local Markdown links, and repository whitespace checks pass.

The namespace matrix covers exact, aliased, and nonrecursive wildcard imports
beneath `cloth`, plus local-source, dependency-alias, import-alias, package, and
case-only shadow attempts. Compiler tests compare whole-project and source-free
artifact consumers on x86-64 and wasm32. The separate-package integration emits
verified LLVM before and after O2 on both targets, links independent x86-64
objects, and executes `Math` natively.

Shuttle rejects absent, corrupt, duplicate-field, unknown-field, unsupported-
schema, selection-mismatched, non-normal, and missing-manifest metadata. It also
rejects wrong-name, wrong-version, executable, and dependency-bearing library
distributions before application compilation. Existing artifact tests close
target, compiler/runtime, receipt, reuse, and link incompatibility. A corrupt
`cloth` candidate is rebuilt without disturbing byte-identical consumers.

An exact `Math.co` edit rebuilds `cloth` and its consumer on both targets, then
returns to full warm reuse. An invalid library edit preserves the completed
library artifact, consumer artifact, and native executable and never launches
stale output. Relocated one-job/four-job artifacts remain byte-identical on both
targets, as do relocated native executables. Compatibility remains artifact/
compiler/runtime **5/5/4** and process/receipt/manifest/toolchain schemas
**2/1/1/1**. **Stage 35 is complete.**

## Stage 35.3 Shuttle standard-library integration

Verified on Windows on 2026-09-05 with development and ASan/UBSan compilers.
Shuttle reads strict schema-1 `cloth-toolchain.json` beside the selected
compiler, validates the exact executable-free `cloth` distribution, injects it
as a direct dependency of every ordinary package, and compiles the library
without a self-edge. Missing metadata, incompatible compiler capabilities,
reserved manifest aliases, and replacement packages fail before application
compilation.

A dependency-free application manifest imports `cloth.math::Math`, checks on
x86-64 and wasm32, records `cloth` in its artifact receipt, links and prints
`6` natively, and reuses both artifacts on a warm build. The existing cache,
parallel, invalidation, relocation, source-free, stale-link, and failure tests
now include the implicit fifth package.

Both compiler configurations pass all 255 CTests, including 34 public Shuttle
toolchain cases and 31 native cases. All 47 ordinary Rust tests, Rust 1.85,
formatting, warning-denied Clippy, and C++ formatting pass. Compatibility
remains artifact/compiler/runtime 5/5/4 and process/receipt/manifest 2/1/1;
toolchain metadata begins at schema 1.

## Stage 35.2 standard library bootstrap

Verified on Windows on 2026-09-05 with development and ASan/UBSan compilers.
The compiler reserves the `cloth` source root, package spelling, import alias,
and dependency alias, accepting only the exact distinguished dependency
`cloth -> cloth`. Focused parser, identity, protocol, package, and negative
case-collision tests cover the compiler-owned boundary.

The `std` checkout is the executable-free `cloth` v0.1.0 package. Its
`src/math/Math.co` source compiles as `cloth.math.Math`; `Gcd` and `Lcm` retain
the Stage 34 checked-division contract through explicit `DivisionByZero`
declarations. A direct Shuttle check compiles the library without an entry point
or self-dependency.

The integration matrix produces and consumes interface artifacts on x86-64 and
wasm32, verifies whole-project LLVM before and after O2 on both targets, and
links independent x86-64 library/application object artifacts. The consumer
opens no library source and prints the exact expected `7`, `9`, and `6` lines.
Both configurations pass all 255 CTests, including the existing 32 public
Shuttle toolchain and 30 native cases. All 43 ordinary Shuttle tests, Rust
formatting, and warning-denied Clippy also pass. Artifact/compiler/runtime
compatibility remains 5/5/4; process protocol, receipt schema, and manifest
schema remain 2/1/1. Shuttle selection and automatic injection remain 35.3
work.

## Stage 35.1 standard library contract

Recorded on 2026-09-05. The approved contract assigns library source to the
`std` repository, retains compiler ownership of primitives and `Error`, reserves
the exact `cloth` import root, and fixes the intended mapping from
`src/math/Math.co` to `cloth.math::Math`. It defines an executable-free library
package, explicit type imports, automatic Shuttle dependency injection, exact
version/digest inputs, compatibility, distribution, diagnostics, verification,
and non-goals.

This checkpoint changes documentation only. Active compiler, runtime, artifact,
process, receipt, and manifest versions remain 5/5/4/2/1/1. Compiler/library
bootstrap is separately scheduled for 35.2; Shuttle production integration is
scheduled for 35.3.

A read-only Stage 34 frontend check of the existing `Math.co` bootstrap reports
two expected 35.2 migration items: dynamic remainder in `Gcd` and dynamic
division in `Lcm` require `DivisionByZero` coverage under the current
conservative effect contract. The checkpoint records these items and does not
weaken error analysis or change library source.

## Stage 34.4 typed error exit audit

Verified on Windows on 2026-09-05 with development and ASan/UBSan compilers.
Both configurations pass all **249 CTests**, including the expanded **18-case**
typed-error target and, per compiler, all **32 public Shuttle toolchain cases**
and **30 native Shuttle cases**. All **43 ordinary Rust tests**, the Rust
**1.85.0** minimum check, warning-denied Clippy, Rust and C++ formatting,
**12 editor grammar/compiler checks per compiler**, all local Markdown targets,
and repository whitespace gates pass.

The audit covers direct, derived, abstract, and sealed errors; constructors and
fields; casts and nullability; zero, one, multiple, base-covered, malformed,
and inaccessible effect sets; recursive private inference; dynamic contract
narrowing; bottom-aware throw contexts; and source-ordered diagnostics. Forged
HIR, MIR error edges, throwing-call result metadata, and ABI state are rejected.

Throwing void, scalar, object, array, enum, and struct success paths verify on
x86-64 and wasm32 before and after O2 and execute natively. Explicit error
messages, integer division/remainder failures, failed construction, stable
terminal status, compiler-known descriptor identity, precise error/message
tracing, and malformed runtime inputs are covered. The audit found and fixed
explicit imports of user error types being mistaken for core-type conflicts and
a constructor field-analysis crash on `throw`; `throw` now contributes proper
non-fallthrough flow without weakening normal-path field initialization.

Format-5 artifacts preserve error kinds, ancestry, effects, and physical ABI.
Whole, separate, and source-free packages agree; relocated serial/parallel
artifacts remain deterministic. A typed-error API edit rebuilds only its package
and consumers, while an invalid effect declaration preserves completed
artifacts and the executable and never runs stale output. Compatibility remains
artifact format **5**, compiler ABI **5**, runtime ABI **4**, process protocol
**2**, receipt schema **1**, and manifest schema **1**.

**Stage 34 is complete.**

## Stage 34.3 typed error lowering and integration

Verified on Windows on 2026-09-05 with development and ASan/UBSan compilers.
Both configurations pass all **241 CTests**, including the **15-case** focused
typed-error target and, per compiler, all **32 public compiler-protocol/toolchain
and 29 native Shuttle cases**. All **43 ordinary Rust tests**, the Rust
**1.85.0** minimum check, warning-denied Clippy, Rust formatting, C++ formatting,
**12 editor grammar/compiler checks**, all local Markdown targets, and
repository whitespace checks pass.

Typed errors now lower through explicit MIR success/error control flow and a
target-neutral result/error ABI. Propagation preserves GC roots and skips every
success-path store after failure; throwing constructors publish no failed
object. Compiler-known `Error` and `DivisionByZero` descriptors, stable native
reporting, and throwing `Main` wrappers are implemented without host exception
handling. Integer division and remainder by zero now propagate
`DivisionByZero`; the other checked runtime failures retain their existing
terminal contracts.

Artifact format **5**, compiler ABI **5**, and runtime ABI **4** preserve error
kinds, inheritance, throws sets, physical signatures, object code, and runtime
requirements. Whole-project, separate-package, and source-free programs agree.
Relocated serial/parallel Shuttle builds produce equal x86-64 and wasm32
artifacts, and native success and uncaught-error behavior is deterministic.
Process protocol **2**, receipt schema **1**, and manifest schema **1** remain
unchanged. VS Code recognizes the three typed-error keywords, and the user
reference documents declarations, effects, propagation, reporting, and the
division-by-zero boundary.

Checkpoint 34.3 is complete. The separately authorized 34.4 audit is recorded
above.

## Stage 34.2 typed error frontend and interfaces

Verified on Windows on 2026-09-05 with development and ASan/UBSan compilers.
Both configurations pass all **233 CTests**. The dedicated typed-error target
passes **15 cases** covering lexical and parser contracts, class-like error
hierarchies, compiler-known errors, constructors, visibility, nullability,
throw expressions, lazy coalescing, bottom-aware flow, explicit public effects,
recursive private inference, constructor and field effects, override/interface
narrowing and intersection, diagnostics, malformed HIR, and the native gate.

The frontend now carries canonical error kinds and throws sets through verified
HIR. Legal long binary and unary expressions use bounded semantic work stacks,
so the existing nesting contract remains clean under sanitizers. Invalid source
does not cascade into internal-verifier diagnostics.

This checkpoint deliberately stops before MIR/native propagation. Typed-error
source is available through `clothc --check`; a native build reports that typed
error lowering is not yet available. Artifact format **4**, compiler ABI **4**,
runtime ABI **3**, process protocol **2**, receipt schema **1**, and manifest
schema **1** remain unchanged. No runtime, artifact format, Shuttle fixture,
editor, or user-documentation changes are part of 34.2.

## Stage 34.1 typed error contract

Approved on 2026-09-04. This documentation-only checkpoint freezes file-wide
`error` types, the compiler-provided `Error` root and `DivisionByZero`, throw
expressions, typed throws sets, explicit public contracts, deterministic private
inference, ordinary-call propagation, failed construction, inheritance and
interface compatibility, entry-point reporting, and division-by-zero migration.

The planned implementation uses a target-neutral result/error channel rather
than C++ exceptions, LLVM personalities, platform unwinding, or source-visible
sentinels. It targets artifact format **5**, compiler ABI **5**, and runtime ABI
**4** while retaining process protocol **2**, receipt schema **1**, and manifest
schema **1**. Active repository constants remain format 4, compiler ABI 4, and
runtime ABI 3 until a separately authorized implementation checkpoint performs
the coordinated transition.

No compiler, runtime, Shuttle, editor, artifact, or user-language documentation
behavior changes in 34.1. Existing Stage 33 verification remains authoritative.
All **101 local Markdown target checks** and repository whitespace gates pass.
Checkpoints 34.2 and 34.3 and the separately authorized 34.4 audit are recorded
above.

## Stage 33.4 numeric literal notation exit audit

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **232 CTests**, including **31 public compiler-
protocol/toolchain and 28 native Shuttle cases** per compiler. All **43 ordinary
Rust tests**, the Rust **1.85.0** minimum check, warning-denied Clippy, Rust and
C++ formatting, **10 editor tests per compiler**, all **100 local Markdown
target checks**, and repository whitespace gates pass.

The focused numeric target now passes **17 cases**. Its exit matrix covers every
integer suffix at binary, octal, and hexadecimal zero, signed minimum, maximum,
and one-past boundaries; lowercase prefixes, both hexadecimal digit cases, and
leading zeroes; and the hexadecimal `f32`/`f64` ambiguity. Scientific values
cover `e`/`E`, signed exponents, integral and fractional mantissas, exact
binary32/binary64 ties-to-even, signed zero, minimum subnormals, exact finite
extrema, near and extreme underflow/overflow, and canonical scalar bits.

Separator tests exercise every permitted digit run and every forbidden
boundary. Invalid prefix, digit, exponent, separator, suffix, and identifier-tail
candidates remain atomic and source ordered. The complete 4,096-byte source
token is checked at and beyond the limit with suffix, prefix, exponent, and
separator bytes included. Existing AST/HIR, forged-HIR, MIR, O2 LLVM,
whole/separate/source-free, invalidation, output-preservation, and relocated
serial/parallel matrices remain clean on x86-64 and wasm32.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and manifest schema **1** remain unchanged.
**Stage 33 is complete.**

## Stage 33.3 numeric literal notation integration

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **232 CTests**, including **31 public compiler-
protocol/toolchain and 28 native Shuttle cases** per compiler. All **43 ordinary
Rust tests**, the Rust **1.85.0** minimum check, warning-denied Clippy, Rust and
C++ formatting, **10 editor tests per compiler**, and all **100 local Markdown
link checks** pass.

Dedicated MIR coverage folds radix arithmetic, scientific `float32`/`float64`,
and `wrap`/`sat` inputs to exact scalar bits, verifies the optimized module, and
emits LLVM from it. Direct x86-64 and wasm32 output passes LLVM verification
before and after O2. Native execution covers static constants, typed overloads,
switch labels, conversions, and the hexadecimal `f32` ambiguity.

The four-package Shuttle fixture agrees across whole-project, separate-package,
and source-free execution. Relocated one-job and four-job builds with reversed
dependency declarations produce identical x86-64/wasm32 interface artifacts
and identical x86-64 native artifacts and executables. A notation-only edit
rebuilds its package and consumer while reusing independent packages; an invalid
base digit preserves completed artifacts and the executable without running it.

VS Code highlights approved scientific, base-prefixed, separated, and suffixed
forms atomically and rejects representative malformed atoms. User integer,
floating-point, syntax, and getting-started documentation now describes the
notation. Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process
protocol **2**, receipt schema **1**, and manifest schema **1** remain unchanged.
At this checkpoint, 33.4 remained separately authorized; its completed audit is
recorded above.

## Stage 33.2 numeric literal notation frontend

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **226 CTests**, including **30 public compiler-
protocol/toolchain and 26 native Shuttle cases** per compiler. The focused
numeric-literal target passes all **14 cases**, and direct `--check` coverage
accepts the new notation for both x86-64 and wasm32.

One bounded decoder now classifies decimal, scientific, binary, octal, and
hexadecimal spellings; validates strict separators and suffix interactions; and
provides canonical values to semantic analysis, constant evaluation, HIR
lowering, and verification. Integer radix conversion and scientific floating
evaluation are exact and target-independent. Malformed candidates are consumed
atomically with category-specific diagnostics, including missing digits,
invalid bases, separator placement, suffix conflicts, and range failures.

The AST retains source text while HIR retains only minimal base-ten integers or
normalized coefficient/exponent floats. Focused mutation coverage rejects
retained prefixes, separators, suffixes, and noncanonical or unrepresentable
values. This checkpoint adds no downstream emission, package, editor, or user-
documentation claim. Artifact format **4**, compiler ABI **4**, runtime ABI
**3**, process protocol **2**, receipt schema **1**, and manifest schema **1**
remain unchanged. The separately authorized 33.3 integration checkpoint is
recorded above.

## Stage 33.1 numeric literal notation contract

Approved on 2026-09-04. This documentation-only checkpoint fixes scientific
notation, lowercase binary/octal/hexadecimal prefixes, strict digit separators,
case and hexadecimal-suffix ambiguity rules, exact value interpretation,
atomic malformed recovery, notation-free canonical HIR, unchanged compatibility
versions, verification requirements, and non-goals.

It changed no compiler, runtime, Shuttle, editor, user-language documentation,
or artifact implementation. The separately authorized 33.2 frontend checkpoint
is recorded above.

## Stage 32.4 typed numeric literal exit audit

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **224 CTests**, including **30 public compiler-
protocol/toolchain and 26 native Shuttle cases** per compiler. All **43 ordinary
Rust tests**, the Rust **1.85.0** minimum check, warning-denied Clippy, Rust and
C++ formatting, and **nine editor tests per compiler** pass. Local links are
valid across all **99 Markdown files**, including all **38 user-documentation
pages**.

The expanded ten-case numeric target covers every suffix at zero and all
integer minima, maxima, neighboring, and out-of-range values. It verifies
signed minima, unsigned rejection, binary32/binary64 ties-to-even neighbors,
signed zero, minimum subnormals, finite extrema, underflow and overflow, exact
canonical bits, unchanged unsuffixed contexts, and the 4,096-byte token limit.
Invalid category, case, width, repetition, alias, and identifier-tail suffixes
are consumed atomically and diagnosed once in source order.

HIR verification rejects retained suffixes, malformed cores, nonnumeric types,
and out-of-range canonical values; invalid source recovery lowers invalid
numeric expressions without verifier cascades. MIR verification rejects an
invalid bit pattern derived from a typed literal. Existing x86-64/wasm32 LLVM,
native execution, source-free package, invalidation, output-preservation, and
relocated serial/parallel determinism checks remain clean. Artifact format
**4**, compiler ABI **4**, runtime ABI **3**, process protocol **2**, receipt
schema **1**, and manifest schema **1** remain unchanged.

**Stage 32 is complete.** The completed Stage 33 audit is recorded above.

## Stage 32.3 typed numeric literal integration

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **224 CTests**, including **30 public compiler-
protocol/toolchain and 26 native Shuttle cases** per compiler. All **43 ordinary
Rust tests**, warning-denied Clippy, Rust and C++ formatting, and **nine editor
tests per compiler** pass.

The integration fixture covers all ten suffixes plus static constants,
assignment, return, array, operator, overload, switch, checked conversion,
`wrap`, `sat`, and exact native output. Canonical typed constants and MIR folds
survive source erasure; raw and O2 x86-64/wasm32 LLVM verify. Shuttle proves
whole-project, separate-package, and source-free equivalence, affected-only
invalidation, failed-output preservation, and relocated serial/parallel
determinism without parsing source syntax. Artifact format **4**, compiler ABI
**4**, runtime ABI **3**, process protocol **2**, receipt schema **1**, and
manifest schema **1** remain unchanged. At this checkpoint Stage 32 remained
active and 32.4 required separate authorization; the completed exit audit is
recorded above.

## Stage 32.2 typed numeric literal frontend

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **216 CTests**, including the new six-case typed
numeric literal target and **29 public compiler-protocol/toolchain and 24 native
Shuttle cases** per compiler.

Focused coverage proves atomic recognition of all ten canonical suffixes,
deterministic malformed-tail recovery, complete AST spelling, exact semantic
types, unchanged unsuffixed defaults, lossless widening, overload and switch
behavior, checked conversions, signed minima, static constant evaluation,
suffix-free HIR lowering, and forged-HIR rejection. No runtime, MIR, LLVM,
artifact, protocol, manifest, receipt, editor, or Shuttle production surface
changed. At this checkpoint Stage 32 remained active and 32.3 required separate
authorization; both later checkpoints are recorded above.

## Stage 32.1 typed numeric literal contract

Approved on 2026-09-04. This documentation-only checkpoint fixes the ten
canonical suffixes, exact initial typing, representability and recovery rules,
suffix-free HIR/MIR boundary, unchanged compatibility versions, verification
matrix, and non-goals. It changes no compiler, runtime, Shuttle, editor, or
artifact implementation. The later 32.2 frontend checkpoint is recorded above.

## Stage 31.4 MIR optimization exit audit

Verified on Windows on 2026-09-04 with development and ASan/UBSan compilers.
Both configurations pass all **215 CTests**, including **29 public
compiler-protocol/toolchain and 24 native Shuttle cases** per compiler. All
**43 ordinary Rust tests**, Rust **1.85.0**, warning-denied Clippy, Rust and C++
formatting, six editor support tests per compiler, documentation, and
repository whitespace gates pass.

The audit compares internal raw and production-optimized pipelines without a
public optimization switch. Native fixtures have byte-for-byte identical
standard output and error, the same process status, and the same side-effect
counts; failing constant division preserves the exact runtime failure. Focused
tests cover every scalar boundary and eligible operator, static and imported
constants, arguments, returns, loops, phis, branches, and switches. They also
prove structural fixed-point idempotence, stable function output across source
insertion order, malformed optimized-MIR rejection, and an iterative
16,384-node SSA worklist.

Raw and optimized x86-64 and wasm32 LLVM modules verify before and after the O2
pipeline. Existing Shuttle whole-project, separate-package, source-free,
affected-only invalidation, failed-output preservation, relocated
serial/parallel, and cross-target determinism cases remain clean. Artifact
format **4**, compiler ABI **4**, runtime ABI **3**, process protocol **2**,
receipt schema **1**, and manifest schema **1** remain unchanged.

**Stage 31 is complete.** The current successor status is recorded above.

## Stage 31.3 MIR CFG and integration checkpoint

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **201 CTests**, including the focused MIR
optimizer suite and the shared Shuttle protocol/native package suites.

Focused coverage proves executable-edge phi propagation, constant branch and
integer/enum switch selection, unreachable-effect removal, stable dense block
and value compaction, post-pass MIR verification, and repeated-pass
idempotence. Production compilation now runs the optimizer between two MIR
verification passes and before ABI lowering. A package fixture proves that
whole-project and source-free dependency compilation fold the same imported
constant and emit the same optimized app function body. Existing Shuttle tests
exercise affected-only invalidation, failed-output preservation,
serial/parallel scheduling, source-free execution, and native package builds
through the public compiler boundary.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and manifest schema **1** remain unchanged.

**Stage 31.3 is complete.** Stage 31 remains active with its 31.4 exit audit
awaiting separate authorization.

## Stage 31.2 MIR scalar-fold checkpoint

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **201 CTests**, including the new focused MIR
optimizer suite and the existing shared Shuttle protocol/native tests.

The focused suite covers canonical integer, boolean, character, enum,
`float32`, and `float64` values; unary/binary operations; widening, checked,
wrapping, and saturating conversions; verified source-owned and source-free
imported static constants; deterministic typed hexadecimal MIR printing; exact
LLVM floating bitcasts; and repeated scalar optimization. Overflow, division by
zero, invalid shifts, failed checked conversion, and non-finite floating
results retain their runtime MIR. Malformed canonical bits are rejected by the
post-pass MIR verifier.

At the close of this checkpoint, the optimizer was not yet in the production
compilation path. Phi propagation, CFG cleanup, default-pipeline integration,
and coordinated package behavior were assigned to the separately authorized
31.3 checkpoint. Artifact format **4**, compiler ABI **4**, runtime ABI **3**,
process protocol **2**, receipt schema **1**, and manifest schema **1** remained
unchanged.

**Stage 31.2 completed with 31.3 still awaiting separate authorization.** The
current Stage 31 status is recorded above.

## Stage 30.4 integer conversion-mode exit audit

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **200 CTests**, including **29 public
compiler-protocol/toolchain and 24 native Shuttle cases** per compiler. All
**43 ordinary Rust tests**, Rust **1.85.0**, Rust formatting, warning-denied
Clippy, six editor checks per compiler, C++ formatting, and repository
whitespace gates pass.

The constant-layer matrix uses an independent signed-magnitude oracle across
all **81 canonical integer source/target pairs**. It covers source extrema and
every target boundary representable by the source: minimum, maximum, zero,
adjacent, in-range, below-range, and above-range values. Both `wrap` and `sat`
are checked for every vector.

A generated Cloth program covers all **121 accepted source/target spelling
pairs**, including `int`, `uint`, and `byte`. Each vector compares a required
`static final` result with the same conversion applied to a runtime parameter.
The program executes natively, and its x86-64 and wasm32 LLVM modules verify
before and after O2. Direct fixtures also cover conversion results in locals,
field initializers, call arguments, returns, and static constants, plus
exactly-once argument evaluation. Existing checked conversion success and trap
tests remain green.

Shuttle fixtures run from distinct project roots and retain byte-identical
serial/parallel x86-64 and wasm32 artifacts. Whole-project, separate-package,
and source-free results agree; conversion edits preserve affected-only
invalidation and unrelated reuse, while failed edits preserve completed
artifacts and never launch stale output. Maintainer and public documentation
now describe the same parser, semantic, HIR/MIR, LLVM, constant, and runtime
contracts.

Artifact format **4**, compiler ABI **4**, runtime ABI **3**, process protocol
**2**, receipt schema **1**, and manifest schema **1** remain unchanged. No
runtime helper, Shuttle production behavior, or test-only compiler switch was
added.

**Stage 30 is complete.** Later work requires a separately approved stage.

## Stage 30.3 integer conversion lowering and integration checkpoint

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **194 CTests**, including **29 public
compiler-protocol/toolchain and 24 native Shuttle cases** per compiler. All
**43 ordinary Rust tests**, Rust **1.85.0**, formatting, warning-denied Clippy,
six editor checks per compiler, C++ formatting, and repository whitespace gates
also pass.

MIR retains wrapping and saturating conversions as distinct verified operations
and rejects non-integer operands or invalid modes. LLVM lowering uses integer
comparisons, selection, truncation, and signed or unsigned extension; it adds no
runtime helper and verifies before and after optimization for x86-64 and wasm32.
Direct native execution covers signed and unsigned narrowing, widening,
cross-signedness boundaries, required constants, and exactly-once evaluation.

Shuttle's public process boundary verifies matching whole, separate, and
source-free package results. Serial and parallel builds produce deterministic
x86-64 and wasm32 artifacts; edits rebuild affected packages while reusing
unrelated ones, and invalid follow-up input preserves completed artifacts and
native output. Shuttle continues to treat conversion object code and constants
as opaque artifact content. Artifact format **4**, compiler ABI **4**, runtime
ABI **3**, process protocol **2**, receipt schema **1**, and manifest schema
**1** remain unchanged.

User documentation now records checked, wrapping, and saturating conversion
semantics in the integer, cast, meta-operation, and static-member references.

**Stage 30.3 is complete.** At that checkpoint, Stage 30 remained active with
the exhaustive 30.4 exit audit awaiting separate authorization.

## Stage 30.2 integer conversion frontend and constant checkpoint

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **188 CTests**, including the unchanged **28 public
compiler-protocol and 22 native Shuttle cases** per compiler. All **43 ordinary
Rust tests**, Rust 1.85 checking, Rust formatting, Clippy with warnings denied,
and all six compiler-backed VS Code tests pass with each compiler. C++ formatting
and both repository whitespace checks also pass.

Parser, semantic, and HIR tests cover `Target::wrap(value)` and
`Target::sat(value)`, contextual operation names, aliases, argument typing,
malformed syntax, non-integer types, and unknown modes. Independent constant and
HIR verification covers signed/unsigned crossings, 64-bit boundaries, invalid
modes, incompatible types, and value-category corruption. Required static
constants pass the complete compilation pipeline with canonical target bits.

Runtime uses are valid through `--check`. At this checkpoint, ordinary
compilation stopped before MIR with a stable diagnostic; the boundary was
regression-tested and did not crash or silently use checked conversion lowering.
The 30.3 checkpoint above supersedes that temporary boundary. Artifact format
**4**, compiler ABI **4**, runtime ABI **3**,
process protocol **2**, receipt schema **1**, and manifest schema **1** remain
unchanged. User-facing language documentation remained deferred at this
checkpoint until native runtime behavior was implemented.

**Stage 30.2 is complete.** At that checkpoint, Stage 30 remained active with
30.3 awaiting separate authorization.

## Stage 29.4 checked runtime arithmetic exit audit

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **186 CTests**, including **28 public
compiler-protocol and 22 native Shuttle cases** per compiler. All **43 ordinary
Rust tests**, the Rust 1.85 baseline, Rust formatting, Clippy with warnings
denied, and all six compiler-backed VS Code tests pass with each compiler.
C++ formatting and repository whitespace checks also pass.

The native success matrix covers `int8`, `int16`, `int32`, `int64`, `byte`,
`uint8`, `uint16`, `uint32`, and `uint64` across `+`, `-`, `*`, `/`, `%`, and
unary `-`, including zero, adjacent, minimum, and maximum endpoints. LLVM tests
require checked signed and unsigned lowering at widths 8, 16, 32, and 64 and
require division/remainder guards before their LLVM operations. They also prove
that float arithmetic, string concatenation, and bitwise operations do not gain
integer-arithmetic guards.

All Stage 29 runtime failures emit exactly one frozen stderr line with nonzero
status and clean stdout. The matrix includes signed-minimum remainder by `-1`
and an ordinary literal-only expression that fails only when executed; the
equivalent `static final` initializer remains a Stage 28 compile-time error.
Independent HIR and MIR verification rejects malformed arithmetic metadata,
operators, result types, and mutability before backend lowering.

The 29.3 whole/separate/source-free, affected-only invalidation, output
preservation, stale-run prevention, runtime-ABI-2 rejection, and relocated
serial/parallel determinism cases remain green. Artifact format **4**, compiler
ABI **4**, runtime ABI **3**, process protocol **2**, receipt schema **1**, and
manifest schema **1** are the completed compatibility boundary.

**Stage 29 is complete.** No later stage or deferred feature is assigned or
active.

## Stage 29.3 checked update and package integration checkpoint

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **182 CTests**, including **28 shared public
compiler-protocol and 22 native Shuttle cases**. All **43 standalone Rust
tests**, Rust 1.85 checking, Rust formatting, Clippy with warnings denied, and
all six compiler-backed VS Code tests pass with each compiler.

Prefix/postfix regression vectors freeze their old/new result values. Every
arithmetic compound is exercised natively; member and array targets expose
left-to-right evaluation and exactly-once source calls, including an RHS that
changes the selected array element before its current value is loaded. MIR and
LLVM checks require target then RHS then load/arithmetic/store, with the runtime
guard before every store. Projected imported-struct storage is covered through
the same checked path.

Dedicated prefix, postfix, multiply-compound, divide-compound, and
remainder-compound failures terminate before an unreachable print and therefore
produce clean stdout. Checked update IR verifies after LLVM's default O2 pipeline
on x86-64 and wasm32.

The shared package fixture produces the same result through whole-project,
separate-package, and source-free native execution. Relocated one-job/four-job
builds have identical artifacts on both targets. A dependency body edit rebuilds
that package and its consumer while reusing unrelated packages; a subsequent
invalid edit preserves completed artifacts/executable bytes and never runs stale
code. Compiler-owned runtime-ABI-2 rejection remains covered without exposing
runtime ABI in Shuttle's public protocol.

At the 29.3 checkpoint, Stage 29 remained active for the separately authorized
29.4 exit audit recorded above.

## Stage 29.2 checked integer lowering checkpoint

Verified on Windows on 2026-09-03 with development and ASan/UBSan compilers.
Both configurations pass all **166 CTests**, including the unchanged **27 shared
compiler-protocol and 20 native Shuttle cases**. All **43 standalone Rust tests**,
Rust 1.85 checking, Rust formatting, Clippy with warnings denied, and all six
compiler-backed VS Code tests pass.

The checkpoint adds direct checked arithmetic at integer widths 8, 16, 32, and
64; native fixtures cover every Cloth integer type, signed-minimum literal
formation, all three exact runtime failures, and clean failure output. Unit
coverage verifies signed/unsigned LLVM overflow intrinsics, ordered guards,
unchanged floating operations, MIR type rejection, the runtime helper, and
runtime-ABI-2 artifact rejection. Format-4 artifact goldens now carry runtime
ABI 3 for x86-64 and wasm32.

Shuttle capabilities and receipts do not expose runtime ABI. Their schemas and
production code remain unchanged; artifacts stay opaque and `clothc` owns the
compatibility check. Stage 29.2 is complete. Update/compound integration remains
scheduled for 29.3 and is not activated by this checkpoint.

Tests are executable language and toolchain contracts. The default suite
contains unit, verifier, LLVM, driver, native-output, and expected-failure
tests. Run it after building:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Each CTest case defaults to a 15-second timeout. Native programs launched by a
test script default to 10 seconds, preventing a broken loop backedge from
hanging a development or CI run. Configure `CLOTH_TEST_TIMEOUT_SECONDS` to
change the outer limit.

The shared Shuttle suites have a separate 1,200-second CTest limit, including
Cargo startup and compilation; each child process inside them has a 300-second
limit. CTest serializes their Cargo access even under parallel test execution.

Test ownership follows the behavior being exercised:

- `tests/unit/` contains C++ unit and verifier executables.
- `tests/integration/programs/` contains standalone Cloth programs.
- `tests/integration/projects/` contains complete multi-file projects.
- `tests/integration/errors/` contains compiler-failure inputs.
- `tests/integration/expected/` contains native-output golden files.
- `tests/fuzz/` contains opt-in fuzz targets and curated source seeds.
- `shuttle/tests/` owns the shared multi-package fixtures and process-protocol
  runner. CTest invokes it against the compiler from the current build tree.

The folder-local `CMakeLists.txt` files own their targets. Native output and
runtime-failure cases share `tests/integration/RunProgram.cmake`; test-specific
launch scripts should not be added. CTest labels allow focused runs with
`ctest --preset dev -L unit` or `ctest --preset dev -L integration`.

## Shuttle and compiler boundary

The development and sanitizer presets enable `CLOTH_TEST_SHUTTLE`. Initialize
the `shuttle` submodule and install Rust 1.85 or newer with Cargo. A missing
checkout or Cargo executable is a configure error rather than a skipped suite.
`CLOTH_SHUTTLE_SOURCE_DIR` may point to another compatible Shuttle checkout.

```sh
ctest --preset dev -L toolchain
ctest --preset sanitize -L toolchain
```

The protocol suite always runs when enabled. Native execution tests are also
registered when LLVM `llc` is available, using the compiler's configured linker
and runtime. A complete toolchain exit audit requires those native tests.
For deliberate compiler-only work, use `-DCLOTH_TEST_SHUTTLE=OFF`; that is not a
complete cross-tool audit.

The shared fixture is a four-package dependency diamond. Tests cover owner-local
aliases, transitive isolation, visibility, equal relative type names, exact
entry selection, stream/status forwarding, malformed protocol requests,
Unicode and space-containing paths, artifact preservation, and relocated IR
and native-byte reproducibility. Tests copy fixtures into temporary directories;
they never build inside the checked-in fixture tree.

Shuttle's ordinary unit and process tests are independent of `clothc`:

```sh
cargo test --manifest-path shuttle/Cargo.toml --all-targets --locked
cargo clippy --manifest-path shuttle/Cargo.toml --all-targets --locked -- -D warnings
cargo fmt --manifest-path shuttle/Cargo.toml --all -- --check
cargo +1.85.0 check --manifest-path shuttle/Cargo.toml --all-targets --locked
```

The real-compiler cases are marked ignored in standalone Cargo runs. CTest
explicitly runs them with `--ignored` and an absolute `CLOTHC_UNDER_TEST` path;
they fail if that path is missing or invalid. See
[Shuttle testing](../shuttle/docs/testing.md) for standalone invocation.

## Sanitizers

AddressSanitizer and, where supported, UndefinedBehaviorSanitizer are opt-in
and cannot be mixed with coverage instrumentation. CMake verifies that the
selected compiler has the required runtimes before generating the build:

```sh
cmake --preset sanitize -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset sanitize
ctest --preset sanitize
```

Compiler and test targets are instrumented. `cloth_runtime` remains
uninstrumented because it is linked into independently generated native Cloth
executables; those programs remain covered by the native golden and failure
tests. Windows Clang runtime discovery and test environment setup are handled
by CMake.

The shared suites launch the instrumented compiler, so protocol parsing,
package compilation, Unicode handling, and entry selection are covered by the
same sanitizer gate. Shuttle itself stays on stable Rust with unsafe code
forbidden; this audit does not claim nightly Rust sanitizer instrumentation.

## Coverage

GNU and Clang coverage instrumentation is available without changing normal
builds:

```sh
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage
```

The build produces the compiler's native coverage data for tools such as
`gcov` or `gcovr`. Runtime instrumentation is excluded for the same native-link
boundary described above.

## Lexer and parser fuzzing

The libFuzzer target requires Clang and sanitizer mode. Seed inputs include
valid and malformed array iteration:

```sh
cmake --preset fuzz -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset fuzz
ctest --preset fuzz
```

For a longer local campaign, invoke `cloth_lexer_parser_fuzzer` with the copied
build corpus at `build/fuzz/tests/fuzz/corpus/lexer_parser` and an explicit time
or run limit. The source tree retains only curated `.co` seeds; hash-named
generated corpus entries stay in the ignored build directory. The target feeds
arbitrary bytes through both lexer and two-pass parser; diagnostics and invalid
input must never crash or violate sanitizer checks.

The presets select build behavior but do not hard-code a compiler. Pass a
compiler on the first configure, as shown for Clang above, or select one through
the environment or a CMake toolchain file.

## Stage 22 exit audit

Verified on Windows on 2026-08-31 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- 89/89 CTest entries pass in each configuration, including all 17 shared
  protocol/native cases;
- all 34 standalone Shuttle tests pass;
- C++ formatting and warnings-as-errors, Rust formatting, Clippy with warnings
  denied, and the Rust 1.85 MSRV check pass;
- relocated native builds are byte-identical in both linker configurations;
- direct compilation works without reading either manifest filename; and
- malformed configuration fails before compiler invocation, while source
  diagnostics and supported exit statuses cross the process boundary intact.

The Unix-only symbolic-link case remains platform-conditioned and was not run
on this Windows host. Remote dependencies, separate artifacts, caching, and
additional native targets remain outside Stage 22's approved scope.

## Stage 23.2 exit audit

Verified on Windows on 2026-08-31 with the GNU development compiler and the
Clang ASan/UBSan compiler:

- 92/92 CTest entries pass in each configuration, including the 17 shared
  Shuttle protocol/native cases;
- the new identity suite fixes encoded byte/hash vectors and checks aliases,
  relocation, source order, exact versions, domain separation, nullability,
  package validation, and corrupted descriptor/initializer ABI metadata;
- the imported-package suite verifies detached ownership, private declaration
  retention, reachable type closure, nullable overload identity, static literal
  values, inheritance/interface layouts, constructor initializer signatures,
  canonical ordering, and corrupted cross-record rejection;
- the package-artifact suite fixes SHA-256 and canonical schema vectors, verifies
  byte-identical interface/object round trips and exact target compatibility,
  and rejects oversized, truncated, corrupted, noncanonical, structurally
  invalid, or wrong-machine artifacts before they become imported views; and
- existing native inheritance, interface, GC, entry, and deterministic-output
  tests pass with ABI-2 names and external descriptor/initializer ownership.

Both builds use warnings as errors; the C++ `check_format` target and both
repositories' `git diff --check` also pass.

Package-scoped LLVM unit coverage checks owning definitions, external
dependency declarations, private-body exclusion, and rejected entry options.
It uses a verified whole-project graph. At the Stage 23.2 checkpoint,
protocol-v2 orchestration and separate native linking remained outstanding;
the next audit records that pipeline work.

## Stage 23 exit audit

Verified on Windows on 2026-09-01 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- 92/92 CTest entries pass in each configuration, including all 14 protocol
  and seven native Shuttle/`clothc` cases;
- whole-project and separate compilation produce equal imported semantic/ABI
  records and canonical linker-symbol ownership for the covered package graph;
- the native equivalence graph covers cross-package construction, inherited
  fields, `super`, abstract and covariant functions, final overrides,
  interfaces, checked casts and `::typeName`, nullable flow, arrays, strings,
  and a 2,000-object cross-package GC chain with identical output and status;
- consumer packages compile from validated artifacts after every dependency
  source tree is removed, while rejected programs retain the same diagnostic
  categories and use logical artifact declaration locations;
- missing closures, wrong identities, wrong digests, corrupted payloads,
  duplicate artifact inputs, and output/input aliasing fail without replacing
  prior artifacts or executables;
- relocated native builds produce byte-identical executables and package
  artifacts from identical inputs, and both `x86_64` and `wasm32` interface
  paths remain covered; and
- all 36 ordinary Shuttle tests, Rust 1.85 checking, Rust formatting and Clippy
  with warnings denied, C++ formatting and warnings-as-errors, and repository
  whitespace checks pass.

Stage 23 is complete. Automatic reuse across commands, remote artifacts,
registries, additional native targets, and ABI stability across compiler
releases remain deliberately outside its contract.

## Stage 24.2 responsiveness checkpoint

Verified on Windows on 2026-09-01 with release Shuttle and the GNU development
compiler. The repeatable one-package `examples/Shuttle.toml` `wasm32` check fell
from an initial 9.35-second observation to a 161.9-millisecond median across
five warm-file-system runs. The compiler capability query within that path fell
from 5.86 seconds to a 70.2-millisecond median. This is a 98.3% end-to-end
reduction without changing exact artifact or tool identity.

Shuttle now reports preparation, every package, linking, completion time, and
program launch on standard error by default; `--quiet` suppresses successful
progress without suppressing compiler diagnostics or program streams. All
92 development and 92 sanitizer CTest entries pass, along with all 37 ordinary
Shuttle tests, the Rust 1.85 baseline, Rust formatting and Clippy, C++ formatting,
and repository whitespace checks. At this checkpoint, Stage 24.1 and 24.2 were
complete; validated reuse and deterministic parallel scheduling remained active.

## Stage 24.3 reuse checkpoint

Verified on Windows on 2026-09-01 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler. Protocol version 2 now validates reuse
against the exact current source inventory, dependency artifact closure,
compiler, target, runtime, and native tools before returning a receipt. A clean
status-3 miss has empty streams and is the only path from validation to
compilation.

Shuttle persists immutable, atomically published manifest/receipt records and
requires both an exact manifest snapshot and a matching compiler receipt.
Tests cover unchanged interface/object graphs, manifest-only invalidation,
source and dependency invalidation, target isolation, changed compilers,
runtime/tool compatibility, malformed state, corrupt candidate repair, entry
failures, and concurrent interface/object writers.

All 92 development and 92 sanitizer CTest entries pass, including all 17
protocol and eight native Shuttle/`clothc` cases. All 40 ordinary Shuttle tests,
the Rust 1.85 baseline, Rust formatting and Clippy, C++ formatting, and
repository whitespace checks pass. Stage 24.3 is complete; bounded deterministic
parallel scheduling remains Stage 24.4.

## Stage 24.4 parallel scheduling and exit audit

Verified on Windows on 2026-09-01 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler. Shuttle now bounds package compiler
processes with a positive `--jobs` value, defaults to available host parallelism,
and schedules canonical dependency levels. Candidate validation and compilation
remain separate phases, so every long compiler operation has preceding progress.

Compiler diagnostics are drained into private per-process spools and replayed
unchanged in canonical order. A process barrier proves that the independent
fixture packages overlap with two jobs, while reversed wall-clock failures still
select the exact `--jobs 1` diagnostic. Real-compiler diagnostics are byte-equal
between serial and parallel checks. Relocated one-job and four-job native builds
produce byte-identical package artifacts and executables.

All 92 development and 92 sanitizer CTest entries pass, including all 18
protocol and eight native Shuttle/`clothc` cases. All 43 ordinary Shuttle tests,
the Rust 1.85 baseline, Rust formatting and Clippy with warnings denied, C++
formatting, and both repositories' whitespace checks pass. The Stage 24.2
responsiveness baseline and Stage 24.3 unchanged-build coverage remain intact.
Stage 24 is complete.

## Stage 25 enum exit audit

Verified on Windows on 2026-09-02 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- all 95 CTest entries pass in each configuration, including 20 real-compiler
  protocol tests and nine native Shuttle/`clothc` tests;
- enum tests cover always-public case spellings, case-sensitive names, nominal
  package identity and source order, private type access, overloads, required
  initialization and early returns, static constants, fields, arrays, iteration,
  interface calls, and exactly-once output/meta evaluation;
- the parser accepts exactly 65,536 cases and diagnoses the next case, while
  malformed bodies and unsupported operations fail deterministically;
- HIR/MIR/ABI corruption tests reject invalid enum literals, numeric operations,
  missing initialization, and reference representation; imported-view and byte
  codec tests reject wrong tags/owners, duplicate and keyword names, empty case
  sets, forged private-case metadata, invalid static values, and GC metadata;
- format-2 artifacts round-trip, preserve cases with dependency sources removed,
  invalidate consumers after case edits while reusing independent packages, and
  produce byte-identical one-job/four-job package artifacts with native behavior
  equal to whole-project compilation; and
- all 43 ordinary Shuttle tests, Rust formatting, Clippy with warnings denied,
  Rust 1.85 checking, C++ formatting/warnings-as-errors, and both repositories'
  whitespace checks pass.

Repeatable gates are `cmake --build --preset dev`, `ctest --preset dev`,
`cmake --build --preset sanitize`, `ctest --preset sanitize`, and the standalone
Rust commands in `shuttle/docs/testing.md`. Native enum fixtures reuse the
existing integration runner; no additional launch scripts are introduced.

Stage 25 is complete. Artifact format 2 and compiler ABI 3 require rebuilding
older packages; runtime ABI 1 and process protocol 2 are unchanged. Attached
constant data, runtime payload variants, structs, matching, reflection, and
nullable enum values remain deliberate backlog work.

## Stage 26.2 struct frontend checkpoint

Verified on Windows on 2026-09-02 with GNU development and Clang/MSVC-library
ASan/UBSan builds. All **100 CTest entries pass in each configuration**, including
the existing shared Shuttle protocol and native suites. C++ formatting,
warnings-as-errors, and both repositories' whitespace checks pass.

`cloth_struct_tests` covers nominal identity and source order, alias imports,
constructor/member visibility, complete initialization and early exits, final
inline paths, temporary mutation, read-only receivers, mutable reference
boundaries, parameter/iteration bindings, class/interface signatures carrying
structs, operation restrictions, and inline-layout cycles. Negative HIR checks
cover nominal substitutions, location categories, receiver contracts, invalid
equality operations, and scalar aggregate representation. At that checkpoint,
native, ABI, and artifact paths were tested to reject struct compilation.

The then-two-file `tests/integration/projects/structs` fixture exercised `--check`
with both x86-64 and wasm32 selections; separate driver tests rejected native
lowering and conflicting output options. No new testing launch script was needed.

That checkpoint closed **26.2 only**, not Stage 26. Runtime copy/equality/output
behavior, aggregate calls, GC safety, and source-free struct artifacts remained
for 26.3/26.4. Artifact format 2, compiler ABI 3, runtime ABI 1, process protocol
2, and manifest schema 1 were unchanged at that point.

## Stage 26.3 aggregate implementation checkpoint

Verified on Windows on 2026-09-02 with the GNU development compiler and the
Clang/MSVC-library ASan/UBSan compiler:

- all **121 CTest entries pass in each configuration**, including 21 real-compiler
  Shuttle protocol tests and ten native Shuttle tests;
- aggregate ABI tests cover x86-64 and wasm32 inline layout, flattened private
  reference maps, physical receiver/parameter/result modes, empty values, and
  limits on fields, nesting, value size, reference maps, and frame storage;
- malformed MIR/ABI/imported metadata is rejected, including incomplete
  construction, repeated final initialization, invalid storage roots, wrong
  calling modes, false maps, inline cycles, and forged dependency layouts;
- format-3 interface artifacts have frozen complete-byte lengths and SHA-256
  hashes for both targets, with byte-identical re-encoding and source-free
  private-layout consumption;
- native fixtures cover the `Data(uint64)` constructor/getter, independent
  copies, shallow managed references, nested mutation, exactly-once location
  evaluation, readonly receiver and argument snapshots, fieldwise equality
  including NaN and nullable strings, output/meta, and class/interface calls;
- a test-only native harness inserts collection before allocation and exercises
  loop-carried aggregate phi copies; constructor, local, argument, result, array,
  and captured-owner references survive those safepoints;
- runtime tests cover multi-reference array elements, interior root slots, and
  malformed metadata/overflow traps through the existing program runner;
- Shuttle builds native and wasm32 aggregate dependencies without reopening
  their sources; separate native output matches whole-project output, and old
  format-2 capabilities/receipts are rejected; and
- all 43 ordinary Shuttle tests, Rust formatting, Clippy with warnings denied,
  Rust 1.85 checking, C++ formatting/warnings-as-errors, and both repositories'
  whitespace checks pass.

Frontend/HIR checks remain in `cloth_struct_tests`; layout/package/MIR checks
live in `cloth_aggregate_tests`. The native stress harness uses the existing
integration runner and production verification/emission pipeline rather than
adding a runtime GC-testing API or a new launch script.

That checkpoint closed **26.3 only**. Artifact format 3, compiler ABI 4 (`_C4`),
and runtime ABI 2 require rebuilding older packages. Process protocol 2, receipt schema 1,
and manifest schema 1 remain unchanged. The remaining coordinated exit work was
verified in 26.4 below.

## Stage 26.4 struct exit audit

Verified on Windows on 2026-09-02 with GNU development and Clang/MSVC-library
ASan/UBSan builds. All **121 CTest entries pass in each configuration**, including
all **22 shared compiler-protocol tests and 12 native Shuttle tests**. The CTest
entry count is unchanged because coverage was added to the existing suites.

- Native and forced-GC fixtures now also exercise constructor overloads and
  early returns, struct-parameter overload resolution, inherited/interface
  dispatch and `super` calls carrying values, inherited aggregate fields,
  final/reference mutation boundaries, private-field equality, arrays of empty
  structs, and exactly-once output/meta evaluation. The same sources pass LLVM
  verification for x86-64 and wasm32.
- Aggregate MIR corruption tests reject missing, duplicate, and non-predecessor
  phi edges, scalar inputs to aggregate phis, and phis after ordinary
  instructions. Existing HIR, initialization, ABI, map, cycle, and resource
  boundary regressions remain green.
- Relocated `--jobs 1` and `--jobs 4` struct builds produce byte-identical
  interface artifacts on both targets and byte-identical native artifacts and
  executables on x86-64, including across a PE timestamp boundary. Native output
  matches whole-project compilation before and after dependency edits.
- Adding a private struct field changes layout/reference offsets; adding a
  member changes the declaration contract. Both edits rebuild `data-models` and
  its consumer while reusing byte-identical `foundation` and `tools` artifacts.
  Subsequent unchanged builds reuse all four packages. These checks run for
  interface artifacts on both targets and native objects/execution on x86-64.
- Source-free consumers retain private layouts, overloads, aggregate constructor
  arguments/results, inherited fields, and interface/base calls. Private
  constructors, fields, and methods remain inaccessible after dependency sources
  are removed; failed consumer compilation preserves its previous artifact.
- The audit found and fixed one diagnostic-contract mismatch: aggregate resource
  violations were rejected but classified as invalid models/malformed metadata.
  Typed limit issues now survive imported-model and artifact validation, and
  value/descriptor map counts are checked before constructing decoded offset
  vectors. Re-signed oversized-map artifacts and oversized writer models cover
  the fix without changing the existing golden bytes or compatibility versions.

All 43 ordinary Shuttle tests, Rust 1.85 checking, Rust formatting, Clippy with
warnings denied, C++ formatting/warnings-as-errors, and both repositories'
whitespace checks pass. Existing fixtures and runners were extended; no new
test launch scripts or production testing switches were introduced.

**Stage 26 is complete.** Artifact format 3, compiler ABI 4, runtime ABI 2,
process protocol 2, receipt schema 1, and manifest schema 1 are unchanged by
26.4. Mutating receivers, struct conformance/boxing, reference returns, nullable
struct values, aggregate constants, and other listed non-goals remain deferred.
No later stage is implicitly activated.

## Stage 26.5.1 explicit interface overrides

Verified on Windows on 2026-09-02: **122/122 development and 122/122 ASan/UBSan
CTests pass**, including 22 shared protocol and 12 native Shuttle tests. All 43
ordinary Rust tests, Rust 1.85 checking, formatting, Clippy with warnings denied,
and C++ formatting/warnings-as-errors pass.

The focused override suite covers direct/transitive and multiple contracts,
overloads, abstract restatements, final sealing, inherited implementation reuse,
class/interface targets together, covariance, invalid modifiers/signatures, and
base-only `super`. Reversed source registration exposed a covariance ordering
bug: class interface closures are now completed before base-first override
validation. Both source orders pass.

Imported-package tests reject missing/spurious markers and forged replaced-class
identities, while accepting interface-only overrides without a base target.
Shared protocol tests remove dependency sources, reject missing/unmatched
markers without replacing completed output, and accept the corrected consumer.
Existing native/equivalence fixtures retain class and interface dispatch.

VS Code contributes the existing `override` modifier and an editable function
snippet. All three Node support tests pass with each compiler, including actual
compilation of the snippet and rejection after removing its marker. On Windows,
the sanitizer run needs the Clang ASan runtime directory on `PATH`, as in CTest.
Legacy new-file generator alignment remains explicitly deferred in `TODO.md`.

**26.5.1 is complete.** Existing interface implementations must add `override`;
inherited implementations need no redeclaration. No `impl` keyword, artifact
schema, physical ABI, process protocol, or manifest revision was introduced.

## Stage 27.2 switch frontend

Verified on Windows on 2026-09-02: **127/127 development and 127/127 ASan/UBSan
CTests pass** (30 unit and 97 integration entries). The shared suites retain
22 protocol and 12 native Shuttle tests. All 43 ordinary Rust tests, Clippy with
warnings denied, Rust 1.85 checking, and C++/Rust formatting checks pass. All six
VS Code grammar/snippet checks pass with both compilers.

`cloth_switch_tests` covers grouped/scoped arms, parser recovery, selector/label
restrictions, typed widening and nominal enum identity, duplicate normalization,
64-bit endpoints, missing cases, defaults, return completeness, nearest-target
break/continue, nullable joins, and required/final class/struct fields. Source-free
declaration views retain imported enum and scalar-constant labels. Explicit
literal conversions already retained as scalar values are normalized consistently;
unretained unary static initializers are not promoted into new constant forms.

The suite reaches the combined maximum of 65,536 enum cases/labels and 65,537
arms including default. It checks integer no-match flow even with every uint8
value listed, one-over-limit source rejection, and forged HIR selectors, constants,
duplicates, defaults, counts, symbols, block targets, sharing, and coverage flags.
Enum-case binding is indexed rather than repeatedly scanning the declaration set.

At the 27.2 checkpoint, direct `--check` succeeded on both target layouts while
LLVM emission and direct MIR lowering explicitly rejected switch HIR. The 27.3
audit below supersedes that temporary gate. Compiler-backed editor tests and the
shared protocol suite retain failed-output preservation checks with invalid source.

Both compiler protocols use lexer keyword classification. Shuttle's sorted keyword
list includes `switch`, `case`, and `default`; ordering of integer keywords was
also corrected. Package names, artifact format 3, compiler ABI 4, runtime ABI 2,
and process/receipt/manifest schemas are unchanged.

## Stage 27.3 switch lowering

Verified on Windows on 2026-09-02: **139/139 development and 139/139 ASan/UBSan
CTests pass** (30 unit and 109 integration entries). Shared gates include
22 protocol and 13 native Shuttle tests. All 43 ordinary Rust tests, formatting,
Clippy with warnings denied, Rust 1.85 checking, and six VS Code tests with each
compiler pass.

The switch suite now verifies typed, sorted MIR tables; exact selector/case
types and enum owners; malformed value/block IDs; out-of-range and duplicate
constants; selector dominance; reachability; trap structure; unique phi inputs;
and constructor initialization across a default return. The combined maximum
of 65,536 enum labels and 65,537 arms reaches LLVM. Its sanitizer run measured
7.5 seconds alone and 12.8 seconds in the suite; a contended run exceeded the
ordinary 15-second limit. Only this boundary suite receives four times the
configured unit timeout (60 seconds by default).

LLVM `opt` accepts x86-64 and wasm32 switch modules. Native execution covers
signed/unsigned 64-bit endpoints, int8/byte bounds, grouped labels, defaults,
integer no-match paths, exactly-once selectors, captured updates, exhaustive
returns, struct construction, and nested loop/switch break/continue targets.
The native harness installs verified mixed scalar/aggregate phis, including
shared case/default targets and a guarded enum default, and forces collection
at every generated allocation boundary. Managed values and GC-bearing structs
survive arm calls, joins, and continues. Separate trap programs corrupt the
native enum producer after MIR verification: exhaustive, default, and default-only
switches must terminate with an illegal instruction, not an ordinary error,
missing executable, or timeout.

Shuttle tests compile switches in both dependencies and consumers, then hide
dependency sources and recompile/link the consumer from artifacts. Nominal enum
aliases, imported enum constants, widened integer constants, and full-width
unsigned constants preserve whole/separate/source-free output. The existing
invalid-output and editor checks preserve completed LLVM files.

Compatibility review: MIR bodies and trap successors are compiler-internal;
declaration records, physical signatures, layouts, and runtime entry points do
not change. Artifact format 3, compiler ABI 4, runtime ABI 2, protocol 2, receipt
schema 1, and manifest schema 1 remain unchanged. Exact compiler identity
invalidates prior cached products.

At the 27.3 checkpoint, dependency evolution and deterministic package builds
remained scheduled for 27.4. The exit audit below closes that work.

## Stage 27.4 switch exit audit

Verified on Windows on 2026-09-02: **141/141 development and 141/141 ASan/UBSan
CTests pass** (30 unit and 111 integration entries). The shared suites pass all
**24 compiler-protocol and 16 native Shuttle tests** with each compiler.

- Source-free consumers reject added cases without coverage, removed/renamed
  case references, duplicate labels caused by enum-constant edits, private
  constants, and labels from another nominal enum. Failed compilation preserves
  the previous consumer artifact. Explicit protocol-v2 requests independently
  reject `switch`, `case`, and `default` aliases without replacing output.
- Native case reordering and integer/enum-constant edits rebuild the producer
  and consumer while reusing byte-identical unrelated packages. An unchanged
  repeat reuses all four packages. Whole-project, separate, and source-free
  execution agree after each valid edit, including changed fallback selection.
- Mixing an old consumer with a changed dependency fails linking without
  replacing the completed executable. Added/removed/renamed cases and
  duplicate-producing constants also leave the previous consumer and executable
  intact after failed `shuttle run`; no stale program output is produced.
  Adding an explicit default repairs an uncovered consumer, and the new case
  reaches that fallback in all three compilation modes.
- Relocated `--jobs 1` and `--jobs 4` builds with reversed dependency declaration
  order produce byte-identical interface artifacts on x86-64 and wasm32, and
  native artifacts/executables on x86-64 across a PE timestamp boundary.
  Added-case diagnostics retain identical text, source positions, notes, and
  ordering after normalizing only the absolute fixture-root prefix; preserved
  artifacts remain byte-identical after both failures.
- Native coverage now executes all 256 int8 labels and 256 sparse uint64 labels
  plus adjacent misses. Nullable facts survive switch joins and forced GC;
  indexed selectors with postfix updates are evaluated once. Diagnostic tests
  check duplicate/note locations and bound missing-case lists in declaration
  order. Existing parser, HIR/MIR corruption, maximum-size emission, phi,
  constructor, scoped-transfer, invalid-tag trap, and GC suites remain green.

All 43 ordinary Shuttle tests, Rust 1.85 checking, Rust formatting, Clippy with
warnings denied, C++ formatting/warnings-as-errors, and repository whitespace
checks pass. All six VS Code grammar/snippet/compiler tests pass with each
compiler. User documentation explains enum evolution and defaults; maintainer
contracts and both stage ledgers are synchronized. Existing test infrastructure
was extended without adding launch scripts or production test switches.

**Stage 27 is complete.** This audit required no production behavior changes.
Artifact format 3, compiler ABI 4, runtime ABI 2, process protocol 2, receipt
schema 1, and manifest schema 1 remain unchanged. Pattern matching, guards,
ranges, switch expressions, general constant folding, and all other recorded
non-goals remain deferred. No later stage is implicitly activated.

## Stage 28.2 typed constant checkpoint

Verified on Windows on 2026-09-02: **142/142 development and 142/142 ASan/UBSan
CTests pass** (31 unit and 111 integration entries), including all **25 shared
compiler-protocol and 16 native Shuttle tests** with each compiler. All 43
ordinary Rust tests, Rust 1.85 checking, Rust formatting, Clippy with warnings
denied, and six VS Code grammar/snippet/compiler tests per compiler pass.

The scalar suite covers exhaustive 8-bit signed/unsigned arithmetic, 64-bit
endpoints, shifts, checked conversions, exact IEEE rounding vectors under four
host rounding modes, signed zero, subnormals, underflow, and non-finite failures.
Frontend tests cover forward/imported/aliased values, privacy, named switch
constants, skipped evaluation versus eligibility, cycles with bounded notes,
reversed source registration, diamonds, and suppression of dependent cascades.

Maximum and one-over cases cover 65,536 declarations, 65,536 initializer nodes,
1,048,576 package nodes, depth 256, and 4,096-byte numeric spellings. Package
budgets span source directories but not independent owning packages. The full
65,536-declaration dependency chain uses iterative evaluation. Depth tests cover
groups, unary/binary/logical chains, checked conversions, and ineligible call and
member paths. They exposed sanitizer stack exhaustion; separating recursive
dispatch from construction/type-checking temporaries fixed it without raising
stack sizes or lowering the language limit. The resource suite runs serially
with the existing extended test timeout.

Public CLI/protocol tests accept new forms through direct `--check`, reject
LLVM/native/interface emission with an explicit checkpoint diagnostic, and
preserve completed outputs. Source-free frontend consumers retain the existing
literal scalar/enum values on x86-64 and wasm32. No launch script, compiler test
switch, external dependency, keyword, or compatibility version was added.

**28.2 is complete; Stage 28 is not.** Authoritative output-side values,
independent value-claim verification, and the coordinated format-4 transition
remain 28.3 work; the complete native/source-free/evolution audit remains 28.4.

## Stage 28.3 constant integration checkpoint

Verified on Windows on 2026-09-02: **148/148 development and 148/148 ASan/UBSan
CTests pass** (31 unit and 117 integration entries). These include all **25
shared compiler-protocol and 17 native Shuttle tests** with each compiler.
All 43 ordinary Rust tests, Rust 1.85 checking, formatting, Clippy with warnings
denied, and six VS Code tests per compiler pass. C++ formatting and repository
whitespace checks pass.

Static fields carry canonical typed bits through semantics, HIR, MIR, LLVM,
and owned imported declarations. No returned-MIR-literal recovery, executable
static initializer, floating decimal round trip, or temporary emission gate
remains. Existing runtime expression lowering is unchanged. HIR independently
checks source/value agreement, types, ownership, and dependency cycles; MIR and
imported-model checks reject missing, malformed, or inconsistent scalar claims.

Coverage includes signed endpoints and full-width unsigned values; bool/char,
exact float bits, signed zero and subnormals; checked conversions and skipped
evaluation; private/public and cross-package dependencies; aliases, inherited
lookup, and integer/enum switch labels. LLVM verifies the new constant globals
on x86-64 and wasm32. Whole-project, separate-package, and source-free native
execution agree. A full-suite regression exposed an unsigned-subtraction
type-probe overflow; the verifier now uses safe probe operands and the focused
test preserves that distinction between type validity and evaluated overflow.

Format-4 tests cover exact owned-value round trips, signed mathematical decimal
encoding, noncanonical/out-of-range re-signed metadata, non-finite/invalid-width
float bits, enum ownership/tags, updated golden artifact digests, and rejection
of format 3. Invalid source constants preserve completed LLVM, native, and
interface outputs. Compiler capability and receipt requirements agree with
Shuttle without adding metadata interpretation to it.

The maximum-size fixtures also pass the new HIR verification. They run serially
with a 120-second default test timeout for sanitizer headroom; language limits
and host stack sizes are unchanged. Existing test infrastructure was extended
without new launch scripts, dependencies, keywords, or production test switches.

**28.3 is complete; Stage 28 remains active.** Artifact format **4** is required;
compiler ABI **4**, runtime ABI **2**, protocol **2**, receipt schema **1**, and
manifest schema **1** are unchanged. Rebuild older artifacts. Constant-specific
dependency evolution, stale links, relocated serial/parallel determinism, and the
remaining comprehensive boundary audit stay scheduled for **28.4**.

## Stage 28.4 scalar constant exit audit

Verified on Windows on 2026-09-02: **148/148 development and 148/148 ASan/UBSan
CTests pass** (31 unit and 117 integration entries). The runs include all **27
shared compiler-protocol and 20 native Shuttle tests** with each compiler.
All **43 ordinary Rust tests**, Rust 1.85 checking, formatting, Clippy with
warnings denied, and **six VS Code tests per compiler** pass. C++ formatting
and whitespace checks pass in the compiler and affected submodules.

The exit audit found and corrected two executable regressions:

- Nested unary signs were cancelled before evaluation, allowing a signed-minimum
  negation overflow to disappear. The innermost sign forms a literal; every
  outer operation is now checked in source order, including literal conversions.
  HIR re-evaluation follows the same rule. Skipped arithmetic remains unevaluated.
- A valid float32 literal at a rounding boundary emitted decimal LLVM IR that
  `opt` and `llc` rejected. Runtime literal and numeric-bound emission now retains
  the resolved IEEE bits in constant bitcasts. Literal parsing, numeric typing,
  runtime arithmetic policy, and optimization scope are unchanged.

Independent verification now also checks grouped/signed literal child types,
presence flags, and character escape spellings. Tests exercise malformed HIR
graphs, initializer/count/spelling limits, excessive imported constants and MIR
claims, re-signed wrong-kind/bool/character records, and the existing signed,
float, enum-owner/tag, old-format, and integrity failures. The full declaration
limit passes MIR verification; maximum/one-over parser/evaluator budgets and
deep/diamond graphs continue to pass under sanitizers without larger stacks.

Exact expected integer and IEEE encodings cover every scalar width across
x86-64 and wasm32, including recurring quotients, widening, ties, signed zero,
subnormals, and underflow. No host-double oracle supplies expected float values.
LLVM verifies both targets; native checks agree with ordinary runtime arithmetic,
shifts/bitwise operations, conversions, and rounding. Static fields remain data,
without executable initializer bodies or startup/GC registration.

Shared regressions prove computed/private/transitive edits rebuild affected
consumers; a source edit with the same evaluated value still changes the artifact
identity. Unrelated packages reuse unchanged bytes, warm builds reuse every
package, and stale consumer links fail without replacing the executable.
Invalid arithmetic, cycles, narrowing/private access, and duplicate integer/enum
labels preserve completed outputs; failed `run` never executes the stale program.
Whole-project, separate, and source-free native outputs agree after edits.

Relocated one/four-job builds with reversed dependency declarations produce
byte-identical interface artifacts within each target and native artifacts and
executables on x86-64 across a timestamp boundary. Cycle/evaluation diagnostics
also agree after normalizing only the fixture-root path. Existing test suites and
launchers were extended; no keyword, dependency, test switch, or scheduler change
was needed. Editor grammar remains applicable without modification.

**Stage 28 is complete in both repositories.** User documentation and maintainer
contracts agree. Artifact format **4**, compiler ABI **4**, runtime ABI **2**,
protocol **2**, receipt schema **1**, and manifest schema **1** remain unchanged
from 28.3. No later stage or deferred feature is activated.
