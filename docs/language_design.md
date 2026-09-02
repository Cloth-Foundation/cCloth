# Cloth language design constraints

This document records decisions that later compiler stages must preserve. It is
intentionally limited to stable constraints; the complete grammar belongs in a
separate specification.

## Portability

Cloth follows a write-once, use-anywhere model. Platform-specific behavior must
be isolated behind explicit compiler or runtime boundaries. Source meaning must
not depend on host pointer size, path syntax, locale, iteration order, or other
ambient platform state.

## Implicit file types

Every `.co` source file defines one implicit type whose name is the file stem.
The type is a class unless the file uses an explicit file-kind envelope:

```text
User.co -> User
```

Fields, functions, and nested types declared at file scope are members of that
class. Source code never repeats an enclosing `class User { ... }` declaration.
Stage 16.1 optionally permits an unnamed `class { ... }` envelope while keeping
the file stem as the sole class name.
The compiler retains the file-class identity through all compilation stages so
other files can reference `User` as a normal type.

Stage 18 adds `interface { ... }` as the first alternate file kind. The file
stem remains its only type name. Interfaces carry public function contracts but
no fields, constructors, or object layout.

`enum { ... }` selects a named value enum. Its cases form a closed set; the file
stem remains the type name. See [enums](enums.md) for the implemented contract.

`struct { ... }` selects a nominal aggregate value with explicit initialization
and read-only instance receivers. Parsing, semantic checking, and typed HIR are
available through `clothc --check`; native lowering and struct artifacts remain
pending. See [structs](structs.md) for the value and storage contracts.

Constructor declarations use a class-derived name whose capitalization carries
visibility:

```text
User(string name, int32 id) {     // Public.
    // ...
}

user(int32 id) {                  // Private.
    // ...
}
```

For `User.co`, `User(...)` is public, while `user(...)` and `_User(...)` are
private. Calls always use the type name, such as `User(name, id)`. A public
class may expose only private constructors and provide public static factory
functions instead.

The parser must diagnose a file name that cannot form a valid Cloth type name.
Package directory components follow the same identifier grammar.

## Single-inheritance identity

An explicit file-class envelope may declare one base file class:

```cloth
import models::Human;

class : Human {
  // User.co still defines User.
}
```

The base name follows ordinary import, alias, package, and capitalization-based
visibility rules. The compiler registers the relationship only after closing
the complete source graph and rejects self-inheritance and indirect cycles in a
deterministic qualified-name order. Stage 16.2 gives the graph a stable
base-prefix object layout and descriptor ancestry. Stage 16.3 requires every
declared derived constructor to select its direct base explicitly with
`Derived(...): Base(...)`; construction allocates once and completes base
initialization before derived fields and body. Stage 16.4 adds inherited public
member lookup, transitive base-reference widening, and descriptor-ancestry
checks for `is` and `as`. Stage 16.5 makes public instance functions virtual,
requires `override func` for inherited signatures, and suppresses virtual
dispatch while fields and constructors initialize. Stage 16.6 adds
`super.Method(...)` as a direct call through the current class's direct-base
view. Stage 17.1 adds `abstract class` envelopes and public bodyless
`abstract func Name(...): Type;` declarations while retaining virtual-slot
identity throughout lowering. Stage 17.2 rejects direct abstract construction
and requires each concrete subclass to resolve every inherited abstract slot.
Stage 17.3 prevents inheritance from `sealed class` and prevents replacement of
an inherited slot after `final override func`, without changing object or
virtual-table layout. Stage 17.4 permits a managed-reference override return
that is assignable to the inherited return. Static call-site typing and the
shared reference representation keep the existing slot and ABI unchanged. See
[inheritance.md](inheritance.md).

## Interface contracts

Interfaces use multiple contract inheritance while classes retain single
implementation inheritance. A class lists conformance after `is`, independently
from its optional class base:

```cloth
class : Human is Named, Serializable {
  // User.co still defines User.
}
```

Conformance is structural at the member boundary but nominal at the type
boundary: a matching public function satisfies a declared interface contract,
while the class becomes an interface subtype only through an explicit or
inherited `is` clause. Concrete classes complete every transitive contract;
abstract classes may defer requirements to a concrete descendant.

Interface values use the same managed pointer as class values. Class and child
interface references widen without representation changes. Checked reverse
conversions use `is` and `as`, and interface calls use deterministic dispatch
tables on the most-derived class descriptor. See
[interfaces.md](interfaces.md).

## Capitalization and visibility

Cloth identifiers are case-sensitive. Visibility is inferred from the first
character of a declaration name:

- An ASCII uppercase letter (`A` through `Z`) makes the declaration public.
- An ASCII lowercase letter (`a` through `z`) or underscore (`_`) makes the
  declaration private.

This rule applies to implicit file classes and their fields, functions,
constructors, and nested types. Constructor names are constrained to the
implicit class name or its lowercase-first and underscore-prefixed private
forms. The rule does not apply to local variables or parameters because those
names are not exported across an access boundary. Public declarations may be
referenced from other file classes through same-package lookup or imports.
Private declarations are visible only within their defining file class and its
nested scopes.

Enum cases are the explicit exception: all are public, including lowercase and
underscore-prefixed names. Enum types retain filename-based visibility, and
case names remain case-sensitive. Public cases never expose a private type.

```text
// User.co defines the public class User.
string Name;                       // Public field.
int32 id;                          // Private field.
func Find(UserId id): User {}      // Public function.
func validate(): bool {}           // Private function.
```

An implicit class receives its visibility from the source file stem, so
`User.co` is public and `user.co` is private. Its constructors retain their own
visibility independently of the file-class name. Until Cloth defines Unicode
identifier rules, only ASCII letter case participates in visibility.

Lowercase core type names such as `string`, `object`, `int32`, and `bool` are reserved
language-provided names rather than declarations, so capitalization does not
make them private. Uppercase names remain the user-defined type namespace;
`String` and `string` are therefore distinct.

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
`float32`. `string` is a core immutable UTF-8 reference type. Its `+` operator
concatenates, `==` and `!=` compare content, and its read-only meta queries are
`::length`, `::byteLength`, and `::isEmpty`. Numeric literals use their expected
integer or floating type when representable and otherwise default to `int32` or
`float64`. Primitive numeric values widen only through the lossless rules in
`numeric_conversions.md`; overload selection prefers exact canonical parameter
types, then requires one uniquely compatible widening or literal-fit candidate.
Intentional numeric conversion uses `NumericType(value)`. Runtime narrowing and
signedness changes are checked and trap when the mathematical result is not
representable; numeric literals are validated at compile time. The syntax is a
numeric operation rather than a primitive constructor, and it does not change
the failure contract of nullable reference `as`.
`object` is the universal non-null managed-reference
type for file classes, strings, and arrays; widening to it is representation
preserving and does not box primitives. References are non-null by default. `T?` is a
distinct nullable reference type: `T` widens to `T?`, while `null` is assignable
only to `T?`. Nullability alone does not distinguish overloads.

Lexical scopes contain `self`, parameters, and locals. A nested block may shadow
an outer name, but declarations in the same scope may not collide. Public
functions may be referenced through their file-class name. Fields require an
instance, including `self` for explicit member access.

## Members and meta queries

`.` performs ordinary declared-member lookup, including `EnumType.Case`.
Those names obey their declaration's visibility, overload, receiver, and
mutability rules; enum cases are always public. `expression::name`
performs a language-defined meta query based on the expression's semantic type.
Meta queries are not declarations: their lower-camel-case names have no
visibility, cannot be shadowed or overloaded, and cannot be called or assigned.
The receiver is evaluated once and must be non-null. Lowering may use a
constant, direct operation, or runtime call without changing this source
contract.

The initial meta set is `array::length` plus `string::length`,
`string::byteLength`, and `string::isEmpty`, where `array` and `string` stand
for value expressions of those types. Every managed reference also exposes
`::typeName`; enum values expose their qualified nominal type name, without
boxing. Arrays report the stable erased name `array`. Package and import paths also use `::`,
but only in declaration syntax; expression postfix `::` is unambiguously a meta
query.

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

Static fields use `static final T Name = value;`, where `T` is a scalar
primitive initialized by a literal or an enum initialized by a direct case.
They have
separate constant storage, do not occupy object layout, and are accessed
unqualified or through their file class. Dynamic static initialization,
mutable static storage, and reference-valued static fields are deferred until
initialization order and collector roots have explicit contracts.

## Nullable references

`?` qualifies the reference immediately to its left. This keeps array
nullability explicit: `T?[]` has nullable elements, `T[]?` is a nullable array,
and `T?[]?` permits both. Primitive, enum, struct, and void types cannot be nullable.

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

Field initializers remain independent executable bodies. Constructor MIR marks
where the backend composes the current class's initializers after an explicit
base-constructor call. MIR must not encode host pointer size, ABI rules, runtime
object headers, or garbage-collector barriers.

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

Classical `for` loops use initializer, condition, and update clauses:

```cloth
for (int32 index = 0; index < values::length; index++) { ... }
for (; ready; attempts++, elapsed += step) { ... }
for (;;) { ... }
```

The initializer is either one local declaration or one expression. Its scope
contains the condition, updates, and body, but ends with the loop. The condition
is optional and otherwise must be `bool`; omission means `true`. Update
expressions run from left to right after body fallthrough or `continue`.
Comma-separated updates are local to this header syntax and do not introduce a
general comma operator. `break` skips the updates and exits the loop.

`+=`, `-=`, `*=`, `/=`, and `%=` require a mutable target and preserve its
exact type. The right numeric operand may widen losslessly to that target;
`%=` is integer-only, and `string += string` appends strings. Prefix and postfix
`++`/`--` accept mutable
numeric locations. Prefix yields the stored result, while postfix yields the
previous value. Member receivers and array/index operands are captured once for
every compound assignment or update. A `final` binding cannot use either form.

Fixed-width integers support `~`, `&`, `|`, `^`, `<<`, and `>>` together with
their compound-assignment forms. Bitwise operands use lossless common integer
typing. Shifts preserve the left operand type, validate the count against that
type's width, and use arithmetic right shift for signed values. Explicit
little-endian and big-endian reads and writes operate on `byte[]`; the complete
portable contract is defined in `integer_binary_data.md`.

## Arrays

`T[]` is a homogeneous, fixed-length non-null reference collection with mutable
elements. Array literals evaluate their elements from left to right and infer
one compatible element type. Different managed-reference types join at
`object`, and a reference literal containing `null` infers a nullable element.
Empty and null-only literals require future contextual typing and are currently
rejected. `array[index]` accepts only `int32`, and
both reads and writes perform runtime null and bounds checks. `::length` is a
read-only `int32` meta query with no visibility. Array equality compares
reference identity.

The array runtime stores element layout and whether elements contain references
without exposing that representation to source or LLVM IR. Reference arrays
are traced element by element; primitive array payloads are not scanned.

Array types are invariant: `User[]` is not assignable to `object[]`. This
prevents storing a non-`User` through a widened array reference. Checked array
casts await reified element-type metadata.

File-class descriptors are immutable compiler-emitted metadata. They retain the
qualified type identity, verified object size and alignment, heap object kind,
and every reference-field offset. This is an ABI and runtime contract only;
roots, tracing, and reclamation have no source-language effect. Generated code
registers reference-valued receivers, parameters, locals, and temporary values
in a precise thread-local shadow stack. Root registration likewise does not
change source semantics.

The initial collector is single-mutator, stop-the-world, non-moving, and
non-generational. Every managed allocation is an automatic safepoint. File
classes, strings, and arrays share one managed registry; sweeping an array
releases its payload, while sweeping a concatenated string releases its owned
UTF-8 buffer. Collection timing and heap thresholds are not observable
language semantics; programs must not depend on object addresses, allocation
order, or reclamation timing.

## Core output and native entry point

`print(T)` and `println(T)` are compiler-provided core intrinsics for all
primitive types, `object`, and `null`. File classes, interfaces, and arrays widen to the
`object` overload. `print` appends nothing;
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
