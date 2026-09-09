# Stage 45.5: Universal Object and value representation

Status: **complete — 45.5d exit audit passed 2026-09-09**.

Stage 45.5 establishes one production object model before self-hosted parser
grammar begins. It replaces the earlier boundary in which `object` accepted
only managed references and primitive boxing remained unscheduled.

See the [compiler roadmap](../../ROADMAP.md) and [work ledger](../../TODO.md).

## 1. Language model

`cloth.lang.Object` is the canonical root of every runtime value that can be
observed as an object. The lowercase keyword `object` remains a permanent
source alias for that exact type; it is not a second root and does not acquire a
distinct canonical or overload identity.

Every class with no explicit base class implicitly derives from `Object`.
Errors retain their compiler-owned `Error` root, which derives from `Object`.
Strings and arrays are compiler-backed managed subclasses. Interface values
are object references whose concrete values derive from `Object`.

Primitive, enum, and struct values retain value semantics and compact unboxed
storage in locals, fields, parameters, results, and arrays. They convert to
`Object` by boxing. Consequently, "everything is an Object" does not mean that
every integer operation allocates. It means every concrete value has a sound
object representation at an object boundary.

`void`, `null`, and an absent nullable value are not objects. `null` is admitted
only by `Object?`. A present nullable value may widen to `Object?`, boxing its
payload when the payload is a value type; an absent value becomes `null`.

## 2. Canonical Object API

`std/src/lang/Object.co` owns this public source surface:

```cloth
class {
  Object() {}

  func Equals(Object? other): bool;
  func HashCode(): uint64;
  func ToString(): string;
}
```

The shown semicolons describe the contract, not new native-method syntax. The
production source uses private compiler-paired bridge calls until Cloth can
express the required runtime operations faithfully. The compiler accepts that
bridge only for the exact `cloth.lang.Object` identity, version, signatures,
and method bodies supplied by its paired standard library.

The methods are public virtual instance functions in the listed slot order.
`Equals` defaults to reference identity, is false for `null`, and is reflexive
for every valid reference. `HashCode` returns a code stable for an object's
lifetime. `ToString` returns a non-null string and defaults to the existing
stable `<qualified.Type>` representation.

Derived implementations may override these methods. An override of `Equals`
must preserve reflexivity, symmetry, and transitivity. If `a.Equals(b)` is
true, `a.HashCode()` and `b.HashCode()` must be equal. Unequal objects may have
equal hashes. Mutation of fields used by equality or hashing while an object is
stored in a hashed collection is invalid use of that collection contract; the
runtime does not freeze user objects.

The `==` and `!=` operators do not become calls to `Equals`. They retain the
existing primitive value, string content, enum value, struct structural, and
managed-reference identity rules. This keeps operator evaluation predictable
and makes value equality an explicit virtual operation at an `Object` boundary.

## 3. Hashing

A hash is a lossy classification, not an object's value, a serialization, an
address, or a printable representation. `ToString()` must never derive output
from `HashCode()`.

Default object hashes use an immutable runtime identity assigned to the managed
allocation. The identity is independent of the current address and therefore
survives a future moving collector. It is process-local and is not a stable
file, network, package, cache, or compiler-build identifier.

Primitive boxes hash their exact wrapper type and canonical payload bits.
Integer aliases use their canonical primitive type: `int` boxes as `Int32`,
`uint` as `UInt32`, and `float` as `Float32`. Floating equality treats positive
and negative zero as equal, so their hashes are equal. NaNs follow Cloth's
existing floating equality. Strings override the default with content equality
and a content-compatible hash. Enum and struct boxes use their canonical
nominal type identity plus their existing value-equality components.

Future hashed collections must apply a collection-local seed or mixing policy
at the table boundary so a public deterministic value hash does not become a
hash-flooding guarantee. Stage 45.5 does not publish a general `Hash` utility
class: `Object.HashCode` is the required value contract, while hash combining,
incremental hashing, stable hashing, and collection seeding require their own
API and threat-model review.

## 4. Primitive object representations

The standard library owns the public hierarchy and documentation; the compiler
and runtime own boxing layout, exact payload access, and the trusted bridges
that cannot yet be written faithfully in Cloth.

```text
Object
  Number (abstract)
    Integer (abstract)
      Byte, Int8, Int16, Int32, Int64
      UInt8, UInt16, UInt32, UInt64
    FloatingPoint (abstract)
      Float32, Float64
  Boolean
  Character
```

Each concrete wrapper is final, stores exactly one immutable payload of its
corresponding primitive type, has a public constructor from that exact type,
and overrides `Equals`, `HashCode`, and `ToString`. `Byte` represents Cloth's
unsigned `byte` range `0..255`; it is distinct from `Int8` and `UInt8` even
though all three occupy one unboxed byte.

Concrete integer wrappers publish exact `MIN_VALUE`, `MAX_VALUE`, `BYTES`, and
`BITS` constants. Floating wrappers publish their exact finite range and bit
width only after the compiler can express the constants without lossy source
rounding. `Number`, `Integer`, and `FloatingPoint` do not promise universal
numeric conversion methods: a method such as `ToInt` would hide narrowing,
overflow, signedness, and floating failure policy already made explicit by
Cloth's checked conversions.

Explicit construction, such as `Int32(10)`, and implicit boxing use the same
canonical representation. String parsing remains `int32::parse(text)` with its
typed `ParseError`; wrapper constructors do not silently add parsing effects.

## 5. Enum and struct boxes

An enum box stores the enum's canonical type identity and tag. A struct box
stores one copied inline value and carries the exact reference-offset map for
its payload. Boxing therefore preserves struct copy semantics and precise
tracing. It does not introduce reference aliasing to the original inline
storage.

Unboxing checks the exact canonical boxed type before copying the payload.
There is no numeric widening disguised as unboxing, no structural type match,
and no pointer to mutable box storage exposed to source code. Numeric widening
or narrowing remains a separate explicit conversion after unboxing.

## 6. Runtime and ABI

The two-word managed header remains opaque. The runtime allocation record gains
a non-address identity used by default hashing. Compiler and runtime metadata
identify the canonical `Object` root and boxed value payload without exposing
native layout to Cloth code.

Every managed descriptor supplies the three root virtual slots. User class
descriptors inherit or replace them through the existing verified virtual-table
machinery. Compiler-owned string, array, error, and value-box descriptors obey
the same dispatch contract. `Object` calls through interface references retain
the concrete receiver and ordinary virtual dispatch.

Boxing is explicit in verified HIR and MIR. The instruction records source
value type, target Object type, payload layout, and canonical runtime identity.
Unboxing is likewise explicit and checked. Verifiers reject boxing `void`,
`null`, absent values, malformed layouts, unrelated wrapper identities,
unrooted allocations, invalid descriptor ancestry, and incompatible package
metadata.

The coordinated implementation targets artifact format **8**, compiler ABI
**7**, runtime ABI **11**, and `cloth` **v0.5.0** while retaining process,
receipt, manifest, and toolchain schemas **2/1/1/1**. These versions advance
only when the root, boxing, runtime, package, and source-free implementation is
published together.

## 7. Printing

Primitive print overloads may continue to format unboxed values directly; the
observable text must equal their wrapper's `ToString()` result. Printing an
object performs dynamic `ToString` dispatch and writes the returned string.
Printing `null` writes `null`. A null string returned by a malformed runtime or
override is rejected at the runtime boundary.

The default object representation remains `<qualified.Type>`. It contains no
address, allocation identity, or hash and is deterministic across equivalent
builds and executions. Primitive wrappers print their actual payload values.
Arrays keep the stable `<array>` fallback until a separately approved array
rendering contract exists.

## 8. Ownership

- The standard library owns `Object`, `Number`, `Integer`, `FloatingPoint`, and
  concrete wrapper declarations, names, constants, and public documentation.
- The compiler owns implicit-root typing, the `object` alias, boxing and
  unboxing conversions, intrinsic binding, verified IR, overload behavior,
  descriptor construction, and package compatibility.
- The runtime owns allocation identities, built-in descriptors, payload
  storage, default equality/hash/string operations, and collector integration.
- Shuttle owns exact selection and transport of the paired `cloth` v0.5.0
  package and invalidates consumers when that package or compatibility tuple
  changes.

No layer discovers an alternate Object declaration by filesystem search, name
similarity, or package alias. A declaration that claims the canonical identity
with the wrong kind or shape is a deterministic compile error.

## 9. Checkpoints

1. **45.5a — Contract (complete).** Freeze the canonical root, lowercase alias,
   unboxed storage, boxing, wrapper hierarchy, equality, hashing, string
   representation, printing, ownership, compatibility, verification, and
   non-goals.
2. **45.5b — Root and dispatch (complete).** Bind the exact standard-library
   `Object`, make it the implicit managed root, implement the three virtual
   slots for all managed descriptors, and preserve direct and source-free
   compilation.
3. **45.5c — Value boxes (complete).** Add verified primitive, enum, struct, and nullable
   boxing/unboxing; complete the standard-library wrapper hierarchy; implement
   value equality, hashes, and formatting; and advance compatibility together.
4. **45.5d — Integration and exit audit (complete).** Close semantic, IR, ABI, GC,
   equality/hash, formatting, both-target, native, source-free, Shuttle,
   bootstrap, sanitizer, editor, documentation, determinism, and repository
   matrices.

Stage 46 parser grammar begins only after 45.5d passes.

## 10. Non-goals

Stage 45.5 does not add generics, operator overloading, user-defined conversion
operators, reflection beyond the existing type-name query, mutable box payloads,
box caches, address access, stable serialization hashes, deep array equality or
rendering, formatting templates, hash collections, a public `Hash` class, or
changes to the two-pass parser architecture. It also does not add `Clone` to
`Object`; copy, snapshot, and deep-copy policy require a separate contract.
