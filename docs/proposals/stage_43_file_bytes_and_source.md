# Proposal: Stage 43 portable file bytes and source representation

Status: **complete — 43.4 exit audit passed 2026-09-08**.

This proposal adds one bounded, binary file-read operation to the canonical
standard library and uses it to establish the first file-backed source
representation in the Cloth bootstrap compiler at `F:\Cloth`.

See the [compiler roadmap](../../ROADMAP.md#stage-43-portable-file-bytes-and-source-representation)
and [work ledger](../../TODO.md#stage-43-portable-file-bytes-and-source-representation).

## 1. Public API and ownership

The public API belongs to the reserved standard-library package:

```cloth
import cloth.io::File;

static func Main() throws IoError {
  byte[] contents = File.ReadBytes("src/Main.co");
}
```

`cloth.io::File.ReadBytes(string path): byte[] throws IoError` is a static
function in `std/src/io/File.co`. It evaluates `path` exactly once and returns
a fresh, non-null `byte[]`. `IoError` remains the existing recursively included
`cloth.lang.errors::IoError`; Stage 43 adds no error hierarchy.

Ownership is divided deliberately:

- the standard library owns the public `File.ReadBytes` declaration;
- the compiler recognizes one private bridge used only by the canonical
  compiler-paired `File` implementation;
- the native runtime owns operating-system handles, reads, and status codes;
- Shuttle carries the selected library and runtime requirements without
  interpreting paths or file contents; and
- `F:\Cloth` owns compiler-specific source files, offsets, tokens, and
  diagnostics.

The standard library does not define a compiler `SourceFile` type. The
bootstrap compiler does not call a platform API or compiler intrinsic directly.

## 2. Path contract

The path is an ordinary non-null Cloth `string`. Relative paths resolve against
the program process's current working directory. An absolute path is passed to
the host using its native absolute-path rules. `Shuttle run` preserves its
existing behavior and the child inherits Shuttle's working directory.

On Windows, the runtime converts the managed Unicode string strictly and opens
the path through a wide-character API. On POSIX systems, the string's canonical
UTF-8 bytes form the native path. A U+0000 scalar cannot be represented in a
native path and throws `IoError("file path contains U+0000")` before an open is
attempted.

Stage 43 performs no lexical normalization, canonicalization, case folding,
separator rewriting, extension inference, environment expansion, or URI
interpretation. `.` and `..`, symlinks, mount points, permissions, and platform
case sensitivity retain host behavior. A symlink is followed normally when its
resolved target is a regular file. Directories and other non-regular objects
are rejected.

Portability means the same Cloth API and byte/result contract exist on every
supported host. It does not claim that every path spelling identifies the same
host object.

## 3. Exact byte semantics

`ReadBytes` reads the opened regular file from byte offset zero through
end-of-file and preserves every byte exactly. It does not decode text, remove a
byte-order mark, translate newlines, append a terminator, or reject embedded
zero or malformed UTF-8 bytes.

An empty file returns a valid zero-length `byte[]`. Every successful call
returns independent array storage. Mutating one result cannot change the file
or another result.

The operation observes one sequential read through one opened handle. Given a
stable file, its result is deterministic. Concurrent external modification is
outside the language's determinism guarantee; an incomplete or structurally
inconsistent read detected by the runtime fails without returning a partial
array.

## 4. Bounds, resources, and failures

The initial whole-file operation accepts at most **67,108,864 bytes** (64 MiB).
This fixed cross-platform limit prevents an untrusted path from requesting an
unbounded managed allocation and leaves larger data to a future streaming
contract. A file known to exceed the limit, or a read that crosses it, throws
`IoError("file is too large")` before publishing a result.

Stable public failures are:

- `file path contains U+0000`;
- `could not open file`;
- `file is not a regular file`;
- `could not read file`; and
- `file is too large`.

The public message contains no native error number, localized host text, or
attacker-controlled path. Empty paths and missing, denied, malformed, or
otherwise unopenable paths use `could not open file`. Errors after opening
close the native handle and publish no array. Allocation exhaustion retains
Cloth's existing deterministic terminal allocation policy rather than becoming
a typed `IoError`.

`ReadBytes` has the declared `IoError` effect. Existing automatic propagation
applies; Stage 43 adds no recovery syntax or unchecked exception path.

## 5. Bootstrap source representation

Checkpoint 43.3 adds `frontend.source::SourceFile` under `F:\Cloth`. It is a
bootstrap class with private path and byte-array fields and this initial public
surface:

```cloth
static func Load(string path): SourceFile throws IoError;
func GetPath(): string;
func GetLength(): int32;
func GetByte(int32 offset): byte;
```

`Load` calls `File.ReadBytes` once. The returned array becomes the source
object's backing storage and is not exposed directly. `GetByte` uses ordinary
checked array indexing. Offsets and lengths are byte-based; no scalar-index
conversion is implied.

The source object preserves the original path spelling for diagnostics. It
does not decode UTF-8, classify line endings, build a line index, or manufacture
tokens. Existing `SourceLocation` and `SourceRange` remain valid while later
lexer work decides how byte offsets map to source lines and columns.

The Stage 43 bootstrap smoke path loads an actual `.co` file, observes exact
length and selected bytes, and sizes the existing `TokenBuffer` from the loaded
length. It still emits only the bounded EOF-terminated token sequence needed to
prove file-to-managed-storage integration. Lexer parity remains a later stage.

## 6. Trusted bridge and runtime ABI

The canonical `File.co` implementation calls a private bridge such as
`__readBytes(path)`. As with `Console.__readLine`, this name is available only
while analyzing the exact compiler-paired standard-library declaration. It is
absent from ordinary lookup, imports, exports, and artifacts and cannot select
an arbitrary native symbol or calling convention.

Checkpoint 43.2 adds one runtime entry point:

```cpp
extern "C" void* cloth_rt_file_read_bytes(
    const void* path, std::uint8_t* status) noexcept;
```

Status values are ABI-owned: `0` success, `1` invalid native path, `2` open
failure, `3` non-regular input, `4` read failure, and `5` size-limit failure.
Only success returns a non-null managed `byte[]`; failure returns null. Unknown
statuses, null required pointers, invalid managed strings, and inconsistent
status/result pairs are internal runtime failures.

The caller keeps the managed path rooted across the runtime call. A successful
result uses the existing canonical `byte[]` header and element layout and is
rooted before another safepoint. Native handles and temporary buffers never
enter the managed object graph. The runtime closes every opened handle on every
exit path and does not retain the path or result.

LLVM declares and calls the bridge on x86-64 and wasm32. Native execution is
required on x86-64; wasm32 verification does not claim a filesystem host for
this stage.

## 7. Packages, Shuttle, and compatibility

Checkpoint 43.1 changed documentation only. At that checkpoint, compatibility
remained artifact/compiler/runtime **7/6/9**,
process/receipt/manifest/toolchain schemas **2/1/1/1**, and `cloth` **v0.3.0**.

Checkpoint 43.2 retains artifact format **7**, compiler ABI **6**, and protocol
and schemas **2/1/1/1**; it advances runtime ABI **9 to 10** and the public
compiler-paired standard library **v0.3.0 to v0.4.0**. Interface and object
artifacts already encode static calls, `byte[]`, typed errors, library identity,
and runtime requirements, so no format change is required.

Direct compilation remains core-only unless the canonical library is supplied.
Whole-project, separate-package, and source-free consumers select the exact
library version and digest through existing mechanisms. File paths, contents,
timestamps, and read results are runtime data and never enter build cache keys
or artifacts merely because a program calls `ReadBytes`.

Shuttle does not open application-requested files during `check` or `build`.
During `run`, the child performs the read under its inherited working directory
and host permissions. Shuttle adds no manifest key, source scanner, permission
policy, path rewriting, or file forwarding.

## 8. Diagnostics

Compile-time diagnostics cover wrong arity or path type, nullable paths without
narrowing, missing or incompatible canonical library input, a malformed
`File.ReadBytes` declaration, invalid `IoError` identity or constructors,
forged bridge calls, malformed MIR result/effect metadata, and incompatible
runtime requirements.

Application source cannot name the private bridge. Invalid source must not
reach LLVM or replace a completed interface, object, native, or executable
output. Runtime failures use the stable messages in Section 4 and never expose
an internal verifier diagnostic.

## 9. Verification matrix

Implementation and exit verification cover:

- relative and absolute paths, spaces, non-ASCII scalars, native separators,
  dot components, symlinks to regular files, empty paths, U+0000, missing and
  denied paths, directories, and non-regular inputs where the host supports
  them;
- empty files, every byte value, embedded zero, BOMs, invalid UTF-8, CR/LF
  combinations, chunk boundaries, exact 64 MiB, one byte over, short reads,
  read failures, and no partial result;
- exact-once path evaluation, fresh array identity, mutation independence,
  checked byte indexing, typed propagation, repeated calls, handle cleanup,
  allocation policy, and GC pressure on path and result;
- private-bridge isolation, canonical-library validation, forged HIR/MIR and
  runtime status/result state, exact runtime requirements, and failure-output
  preservation;
- verified x86-64 and wasm32 LLVM before and after optimization plus native
  x86-64 execution on Windows and POSIX;
- whole-project, separate-package, source-free, serial, parallel, and relocated
  builds, exact reuse, affected invalidation, byte-identical artifacts,
  input-independent caching, stale-run prevention, and deterministic
  diagnostics;
- real `F:\Cloth` loading of an actual `.co` file into `SourceFile`, checked
  access, token-buffer sizing, direct and Shuttle checks, native execution,
  warm reuse, and artifact stability; and
- development, sanitizer, Rust/MSRV, editor, user and maintainer documentation,
  formatting, link, whitespace, and repository quality gates.

Host-specific tests are conditional only where the underlying object cannot be
created portably. They do not weaken the shared API, messages, or bounds.

## 10. Stage plan

1. **43.1 — Contract (complete).** Freeze ownership, API, paths, exact bytes,
   bounds, failures, bootstrap representation, trusted bridge, compatibility,
   diagnostics, verification, and non-goals.
2. **43.2 — File foundation (complete).** Implement the runtime operation,
   compiler bridge, canonical `cloth.io::File`, independent runtime/library
   tests, runtime ABI 10, and `cloth` v0.4.0.
3. **43.3 — Bootstrap integration (complete).** Add bootstrap `SourceFile`, load
   a real `.co` file, integrate its bytes and length with the token buffer, and
   verify direct, package, source-free, and Shuttle behavior.
4. **43.4 — Exit audit (complete).** Close platform, byte, path, error,
   resource, GC, malformed-state, compatibility, determinism, bootstrap, and
   repository matrices. Completed on 2026-09-08 with both 354-test compiler
   configurations, both targets, the real bootstrap project, and all Rust,
   editor, documentation, formatting, and repository gates passing.

No later checkpoint is authorized by approval of 43.3.

## 11. Non-goals

Stage 43 does not add:

- file writes, append, mutation, deletion, copying, moving, metadata, directory
  enumeration, temporary files, permissions, locks, or file watching;
- stream or file-handle objects, seeking, incremental reads, memory mapping,
  asynchronous I/O, cancellation, timeouts, or files larger than 64 MiB;
- a `Path` type, path joining, normalization, canonicalization, globbing,
  environment expansion, sandbox, capability system, or virtual filesystem;
- text-file decoding, encoding selection, newline normalization, BOM policy,
  Unicode validation, byte-to-string conversion, or source line indexing;
- automatic compiler project discovery changes, compile-time file reads,
  source embedding, build-script input tracking, or artifact content hashing;
- a general FFI, native pointer, arbitrary intrinsic, or user-defined runtime
  binding mechanism; or
- a complete self-hosted lexer, parser, AST, diagnostic renderer, or compiler.
