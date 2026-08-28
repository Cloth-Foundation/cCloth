# Cloth Stage 12.2 static members

Stage 12.2 makes instance ownership explicit. A member is instance-owned by
default; `static` makes it belong to the implicit file class instead.

## Functions

Static functions use the same declaration syntax with one modifier:

```cloth
static func Parse(String text): int32 {
    return 0;
}
```

A static function has no `self` binding. It may use parameters, locals, static
members, imported file classes, and core intrinsics. It cannot access an
instance field or call an instance function without an explicit object.

Static functions are called unqualified inside their defining file or through
the owning file class:

```cloth
Parse(text);
Parser.Parse(text);
```

`parser.Parse(text)` is invalid when `Parse` is static. Conversely,
`Parser.Read()` is invalid when `Read` is an instance function.

## Fields

The initial static-field contract is deliberately small:

```cloth
static final int32 Version = 12;
static final bool Debug = false;
```

A static field must be `final`, must have an initializer, and must use a scalar
primitive type with a literal initializer. It may be read unqualified in its
defining file or through its file class. It cannot be accessed through an
object.

This restriction keeps initialization deterministic. Static fields lower to
constant global storage with stable mangled names and never occupy object
layout. Mutable static fields, reference-valued static fields, and dynamic
initializers are deferred until Cloth defines initialization order and garbage
collector root registration.

## Native entry point

`Main` is always static:

```cloth
static func Main() {
    println("Hello, World!");
}
```

The eligible signatures are `static func Main()`,
`static func Main(): void`, and `static func Main(): int32`. Capitalization
makes `Main` public. The generated native adapter calls it without allocating
an object or passing a null receiver.

## Compiler representation

Static ownership is semantic symbol metadata, not part of a value type or
overload signature. MIR retains call syntax while semantic analysis guarantees
that the access form matches the selected callable. ABI lowering omits the
receiver parameter for static functions and records static fields separately
from `AbiClassLayout`. LLVM lowering uses that verified distinction directly.
