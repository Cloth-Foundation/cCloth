# Cloth language design constraints

This document records decisions that later compiler stages must preserve. It is
intentionally limited to stable constraints; the complete grammar belongs in a
separate specification.

## Portability

Cloth follows a write-once, use-anywhere model. Platform-specific behavior must
be isolated behind explicit compiler or runtime boundaries. Source meaning must
not depend on host pointer size, path syntax, locale, iteration order, or other
ambient platform state.

## Implicit file classes

Every `.co` source file defines one implicit class whose name is the file stem:

```text
User.co -> User
```

Fields, functions, and nested types declared at file scope are members of that
class. Source code does not repeat an enclosing `class User { ... }`
declaration.
The compiler retains the file-class identity through all compilation stages so
other files can reference `User` as a normal type.

Constructors use the implicit class name:

```text
User(String name, int32 id) {
    // ...
}
```

The parser must diagnose a file name that cannot form a valid Cloth type name.
Package directory components follow the same identifier grammar.

## Capitalization and visibility

Cloth identifiers are case-sensitive. Visibility is inferred from the first
character of a declaration name:

- An ASCII uppercase letter (`A` through `Z`) makes the declaration public.
- An ASCII lowercase letter (`a` through `z`) or underscore (`_`) makes the
  declaration private.

This rule applies to implicit file classes and their fields, functions, and
nested types. It does not apply to local variables or parameters because those
names are not exported across an access boundary. Public declarations may be
referenced from other file classes through same-package lookup or imports.
Private declarations are visible only within their defining file class and its
nested scopes.

```text
// User.co defines the public class User.
String Name;                       // Public field.
int32 id;                          // Private field.
func Find(UserId id): User {}      // Public function.
func validate(): bool {}           // Private function.
```

An implicit class receives its visibility from the source file stem, so
`User.co` is public and `user.co` is private. A constructor uses the class name
and inherits the class visibility. Until Cloth defines Unicode identifier
rules, only ASCII letter case participates in visibility.

For portable builds, a source graph must not contain qualified file-class
identities that differ only by letter case. The compiler diagnoses these
collisions deterministically even on a case-sensitive host file system.

## Core semantic rules

Compilation closes the entry files' package and import graph before semantic
analysis. Every file class is registered before member signatures, and every
member signature is registered before executable definitions are checked. This
preserves forward and cyclic references without making meaning depend on
discovery order.

`int`, `uint`, and `float` are portable aliases of `int32`, `uint32`, and
`float32`. `String` is a core reference type. General implicit numeric
conversions are not part of the initial language; overload selection uses exact
canonical parameter types. References are non-null by default. `T?` is a
distinct nullable reference type: `T` widens to `T?`, while `null` is assignable
only to `T?`. Nullability alone does not distinguish overloads.

Lexical scopes contain `self`, parameters, and locals. A nested block may shadow
an outer name, but declarations in the same scope may not collide. Public
functions may be referenced through their file-class name. Fields require an
instance, including `self` for explicit member access.

## Two-pass parsing

Parsing is designed as two deterministic passes:

1. Discover the file class and member declarations, including fields, function
   signatures, constructors, and nested type names.
2. Parse definitions and executable bodies using the declarations discovered by
   the first pass.

Both passes operate on the same immutable token stream and report through the
shared diagnostic system. Declaration order must not introduce nondeterministic
behavior.

Syntax and semantic object allocation may move to garbage-collected storage in a
future compiler. The initial parser should keep ownership localized and avoid
exposing allocation details as language or compiler identities.

Semantic model and HIR identities are stable numeric handles. Their allocation
strategy is likewise not part of the language contract and may move to managed
storage later.

## Final bindings

`final` prevents a field, parameter, local, or iteration binding from being
assigned again. It does not recursively freeze referenced objects or arrays.
The qualifier is declaration metadata, not part of type identity or overload
selection.

Final fields are initialized in declaration order or exactly once on every
exit path of each constructor. Constructor-body initialization is restricted
to direct assignments on the current instance and cannot occur in loops. These
rules share one definite-initialization analysis with non-null fields while
retaining the stronger exactly-once final-field contract.

## Static members

`static` moves member ownership from an instance to the implicit file class.
A static function has no `self` binding and no receiver ABI parameter. It may
be called unqualified inside its defining file class or through a file-class
name. Calling it through an object is invalid. An instance function may be
called unqualified only where an implicit receiver exists or explicitly
through an object; calling it through a file-class name is invalid.

Stage 12.2 static fields use the intentionally narrow form
`static final T Name = literal;`, where `T` is a scalar primitive. They have
separate constant storage, do not occupy object layout, and are accessed
unqualified or through their file class. Dynamic static initialization,
mutable static storage, and reference-valued static fields are deferred until
initialization order and collector roots have explicit contracts.

## Nullable references

`?` qualifies the reference immediately to its left. This keeps array
nullability explicit: `T?[]` has nullable elements, `T[]?` is a nullable array,
and `T?[]?` permits both. Primitive and void types cannot be nullable.

Nullable values cannot be used for ordinary member access, indexing, or
iteration until they are narrowed. Every non-null reference field is
initialized by its
declaration or definitely assigned on every constructor exit. It cannot be read
before initialization, and the current object cannot escape or invoke instance
functions until all its non-null fields are initialized. Nullable reference
fields default to `null`. Direct `null` comparisons narrow stable locals and
parameters along proven control-flow paths; logical operators compose facts,
and assignment invalidates them. Fields do not narrow until member effects and
aliasing have an explicit model. The ABI erases `?` to the underlying reference
layout and mangling.

Nullable references may be used directly as conditions, where non-null means
true; prefix `!` tests for null. `receiver?.Field` safely reads a
reference-valued field, `value ?? fallback` lazily supplies an alternative, and
postfix `value!` checks at runtime before producing the underlying non-null
type. Safe calls and safe access to primitive fields remain deferred.

## Path-derived packages

Cloth has no `module` or `package` declaration. A file's directory relative to
the project source root is its package, and its stem remains its implicit class
name. For example, `src/models/User.co` has the stable identity `models.User`.

Imports use identifiers rather than filesystem strings:

```cloth
import models::User;
import services.api::*;
import legacy::User as LegacyUser;
```

Dots traverse package directories, `::` selects one file class, and a terminal
`.*` imports every public file class directly in a package. Imports are
file-scoped, non-transitive, and order-independent. Cycles are permitted because
the complete source graph is closed before declarations and bodies are checked.

## Explicit control flow

Executable HIR lowers to a target-independent MIR before target layout or code
generation. MIR preserves source evaluation order, uses body-local value and
basic-block handles, and ends every block with an explicit terminator. Logical
`&&` and `||` retain short-circuit behavior through branches and phi values.

Field initializers remain independent executable bodies until object layout and
constructor composition are specified. MIR must not encode host pointer size,
ABI rules, runtime object headers, or garbage-collector barriers.

## Void functions

`void` is the canonical return type for a function that produces no value. An
omitted return annotation is exactly equivalent to `: void`; return statements
do not infer a function's return type. Void functions may fall through or use
`return;`, but may not return a value.

Void is not a general-purpose value or storage type. It is invalid for fields,
parameters, locals, array elements, and iteration bindings. A call returning
void is valid as an expression statement but cannot be consumed by another
expression. Constructors remain unannotated: their bodies follow void control
flow, while constructor calls produce the new file-class reference.

## Portable ABI boundary

Target data layout is supplied explicitly and never inferred from the compiler
host. Primitive widths are language properties, while reference width,
alignment, object padding, and endianness belong to the selected target.

File-class fields retain declaration order in object layout. Public
capitalization maps to external linkage and private capitalization maps to
internal linkage. ABI names are deterministic and versioned. Backend-specific
IR must consume the verified ABI instead of independently recomputing layouts
or exported names.

## LLVM lowering

LLVM IR lowering consumes only verified MIR and ABI data. It preserves explicit
control-flow edges and ABI field offsets, uses opaque pointers for references,
and isolates allocation, string construction, and null-receiver traps behind a
small runtime interface. Source meaning must not depend on whether LLVM is
linked into the compiler process or invoked as external tooling.

## Structured loops

`while` conditions must be `bool`, and loop bodies must be braced. `break` exits
the innermost loop. In a `while`, `continue` re-evaluates the condition. Both
control statements are errors outside a loop.

Array iteration uses a declaration followed by `in`:

```cloth
for (var value in values) { ... }
for (int32 value in values) { ... }
```

`var` infers the exact element type; an explicit type must accept the element
type. The iterable expression is evaluated once. The loop binding is a mutable
local copy scoped to the body, so reassigning it does not write the array.
`continue` advances the hidden index before rechecking the loop condition.
Future iteration protocols must preserve this source contract.

## Arrays

`T[]` is a homogeneous, fixed-length non-null reference collection with mutable
elements. Array literals evaluate their elements from left to right and infer
one exact element type; a reference literal containing `null` infers `T?[]`.
Empty and null-only literals require future contextual typing and are currently
rejected. `array[index]` accepts only `int32`, and
both reads and writes perform runtime null and bounds checks. `Length` is a
public read-only `int32` member. Array equality compares reference identity.

The array runtime stores element layout and whether elements contain references
without exposing that representation to source or LLVM IR. This is the first
collection boundary designed for a future tracing collector.

## Core output and native entry point

`print(T)` and `println(T)` are compiler-provided core intrinsics for all
primitive types, file-class objects, and `null`. `print` appends nothing;
`println` appends exactly one line feed, and its zero-argument overload writes
only that line feed. File-class objects use `<qualified.Type>` without an
address. Source members named `print` or `println` shadow the corresponding
intrinsic overload set under normal lexical lookup rules. The exact formatting
contract is documented in
[printing_and_object_representation.md](printing_and_object_representation.md).

A native program contains exactly one public `static func Main` with no
explicit parameters. `Main` may omit its return type, explicitly return
`void`, or return `int32`. A void `Main` produces process status zero; an
`int32` `Main` supplies the returned value. The native adapter calls this
receiver-free function directly.
