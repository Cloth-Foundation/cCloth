# Cloth Stage 15 core object model

Stage 15 introduces `object`, the universal non-null type for managed
references. File-class instances, strings, and arrays widen to `object`
without changing their pointer representation. `object?` adds the ordinary
nullable wrapper. Primitive values do not widen to `object`; boxing remains a
future language feature.

## Assignment and equality

Reference widening is implicit and one-way:

```cloth
User user = User();
object value = user;
object? optional = value;
```

Array types remain invariant. A `User[]` is not an `object[]`, because allowing
that conversion would permit storing a string into a `User[]`. A heterogeneous
array literal whose non-null elements are different managed-reference types
instead infers `object[]`; nullable elements or `null` produce `object?[]`.

Equality on an expression statically typed as `object` is reference identity.
This remains true when the referenced value is a string. Two expressions
statically typed as `string` retain Stage 14 content equality.

## Meta queries

Every non-null managed reference supports `::typeName`, returning an immutable
`string`. File classes return their qualified source identity, strings return
`string`, and arrays currently return the stable erased identity `array`.
Addresses, allocation identifiers, and collector state are never exposed.

Like every meta query, `::typeName` is lower camel case, bypasses member
visibility, cannot be called or assigned, and evaluates its receiver once.
Nullable receivers must first be narrowed or asserted non-null.

## Checked type operations

`is` tests runtime type compatibility and never traps. Its target is
non-nullable:

```cloth
bool isUser = value is User;
bool isText = value is string;
```

`as` is the safe cast. Its target must be nullable, so failure is represented
by `null`:

```cloth
User? user = value as User?;
```

File-class checks compare canonical descriptor identity within the emitted
module. Stage 16.4 additionally follows parent descriptor links, so a derived
object matches every transitive base and a base-typed reference can be checked
against a possible derived runtime type. String checks use the stable runtime
heap kind. `object` checks only for a non-null managed reference. Unrelated
concrete source and target types are rejected statically. `is` does not yet
smart-cast the source binding; use `as T?` followed by existing null narrowing.

Checked array casts are deliberately rejected. Runtime arrays currently retain
only reference-element tracing metadata, not their complete source element
type, so accepting `value is User[]` would be unsound. Reified array metadata
must precede that feature.

## Runtime and collection

All values admitted by `object` already share the two-word managed header.
Widening and nullable wrapping are therefore LLVM pointer-preserving
conversions. `::typeName` creates a managed string that borrows program-lifetime
descriptor bytes. Type checks do not allocate. Heterogeneous `object[]`
payloads are pointer-sized reference arrays and are traced by the existing
precise collector.

Stage 15 does not add inheritance, interfaces, primitive boxing, reflection,
user-defined object formatting, or array covariance.
