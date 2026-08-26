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
Module and directory naming rules remain to be specified.

## Capitalization and visibility

Cloth identifiers are case-sensitive. Visibility is inferred from the first
character of a declaration name:

- An ASCII uppercase letter (`A` through `Z`) makes the declaration public.
- An ASCII lowercase letter (`a` through `z`) or underscore (`_`) makes the
  declaration private.

This rule applies to implicit file classes and their fields, functions, and
nested types. It does not apply to local variables or parameters because those
names are not exported across an access boundary. Public declarations may be
referenced from other file classes, subject to the future module and import
rules. Private declarations are visible only within their defining file class
and its nested scopes.

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

For portable builds, a module must not contain source files whose stems differ
only by letter case. The compiler must diagnose these collisions
deterministically even on a case-sensitive host file system.

## Core semantic rules

Compilation is performed over an explicit set of source files. Every file class
is registered before member signatures, and every member signature is registered
before executable definitions are checked. This preserves forward references
without making meaning depend on input order.

`int` and `uint` are portable aliases of `int32` and `uint32`. `String` is a
core reference type. General implicit numeric conversions are not part of the
initial language; overload selection uses exact canonical parameter types. The
null value is assignable only to reference types.

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

## Explicit control flow

Executable HIR lowers to a target-independent MIR before target layout or code
generation. MIR preserves source evaluation order, uses body-local value and
basic-block handles, and ends every block with an explicit terminator. Logical
`&&` and `||` retain short-circuit behavior through branches and phi values.

Field initializers remain independent executable bodies until object layout and
constructor composition are specified. MIR must not encode host pointer size,
ABI rules, runtime object headers, or garbage-collector barriers.

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

`while` is Cloth's first loop form. Its condition must be `bool`, and its body
must be braced. `break` exits the innermost loop and `continue` re-evaluates its
condition. Both control statements are errors outside a loop. This small core
is sufficient to express general iteration without committing to `for` syntax
or iterator protocols prematurely.

## Core output and native entry point

`print(String)`, `print(int32)`, and `print(bool)` are compiler-provided core
intrinsics. They write the value without appending a newline. Boolean output is
lowercase `true` or `false`. A source member named `print` shadows all intrinsic
overloads under normal lexical lookup rules.

A native program contains exactly one public `Main` function with no explicit
parameters. `Main` may omit its return type or return `int32`; the process exit
status is zero in the first case and the returned value in the second. The
native adapter invokes `Main` as a class-qualified function with no instance.
