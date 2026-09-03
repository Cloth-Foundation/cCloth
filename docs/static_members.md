# Cloth static members

Stage 12.2 makes instance ownership explicit. A member is instance-owned by
default; `static` makes it belong to the implicit file class instead.

## Functions

Static functions use the same declaration syntax with one modifier:

```cloth
static func Parse(string text): int32 {
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

Static fields have compile-time scalar values:

```cloth
static final int32 Version = 12;
static final bool Debug = false;
```

A static field must be `final`, must have an initializer, and must use a scalar
primitive or nominal enum type. It may be read unqualified in its
defining file or through its file class. It cannot be accessed through an
object.

The [scalar constant contract](proposals/stage_28_scalar_constants.md) supports
arithmetic, bitwise/boolean operations, comparisons, checked conversions, and
static constant dependencies in checking, native builds, and package compilation.
Values are evaluated once at their resolved types and retained as canonical bits.

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

Required scalar evaluation lives in `sema/scalar_constants.cc` and
`sema/constant_evaluator.cc`. The former uses bounded unsigned multiword arithmetic
for exact binary32/binary64 rounding, without host floating-point operations or a
new dependency. The latter resolves canonically ordered dependency graphs using
an iterative, memoized traversal; failures do not acquire default values.
Signed-literal evaluation preserves source operation order: only the innermost
sign forms the literal (including signed minima); outer signs are checked unary
operations, never cancelled before evaluation. Skipped operands still undergo
literal/type/dependency validation without evaluating arithmetic failures.

Parsing enforces per-package declaration/node budgets while allocating ASTs.
An iterative structural preflight bounds expressions and rejects unsupported
forms before recursive typing. Bound references are checked separately. Recursive
parser/type-dispatch frames are kept small enough for the approved depth under
ASan/UBSan; HIR independently bounds constant trees before further verification.
HIR retains source initializers alongside verified scalar values and independently
checks their values, types, and dependency graph. MIR carries static scalar data
without initializer bodies; its verifier checks claims against semantics. LLVM
uses the bits directly, including integer-to-float constant bitcasts that preserve
exact IEEE values. Imported declarations restore typed bits without source text.
[Artifact format 4](artifact_schema_v4.md) admits the full signed integer range
without changing the physical ABI or runtime.
