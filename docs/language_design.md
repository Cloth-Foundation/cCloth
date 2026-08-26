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
class. Source code does not repeat an enclosing `class User { ... }` declaration.
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
