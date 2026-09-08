# Cloth Stage 41 nullability contract

Cloth types are non-null by default. One trailing `?` admits `null` and creates
a distinct semantic type:

```cloth
User? selected = null;
int32? count = 3;
Status? status = Status.Ready;
Point? point = Point(1, 2);
```

Primitive, enum, struct, class, interface, string, object, and array values may
be nullable. `void`, `null`, and an already nullable type may not be followed
by `?`. Overloads cannot differ only by nullability.

Array and element nullability are independent. `User?[]`, `User[]?`, and
`User?[]?` therefore mean different things. Nullable value elements such as
`int32?[]` and `Point?[]` are supported.

## Compatibility and inference

- `T` is assignable to `T` and compatible `T?`.
- `null` is assignable only to a nullable type.
- `T?` is not assignable to non-null `T` without a proof, fallback, or
  assertion.
- Numeric and reference widening lift through nullability; narrowing still
  requires an explicit cast.

The lifted conversion preserves absence and converts only a present payload:

```cloth
int16? small = 12;
int32? wide = small;
var inferred = true ? 12 : null;  // int32?
```

Array literals containing compatible non-null values and `null` infer a
nullable element type. Empty and null-only literals still require contextual
typing.

Mutable nullable locals and mutable nullable fields default to `null` when no
initializer is present. Final locals require an initializer. Constructors must
initialize every struct field, including nullable fields.

## Presence and narrowing

Any nullable value can be used as an `if` or `while` condition. The condition
tests presence, not its payload. Both `true` and `false` are therefore present
`bool?` values. Prefix `!`, `&&`, and `||` use the same presence rule. A
non-null value condition is rejected unless its ordinary type is `bool`.

```cloth
func Name(User? value): string {
  if (value == null) { return "unknown"; }
  return value.Name;
}
```

Direct and reversed null comparisons, parentheses, logical negation, and
short-circuit expressions compose flow proofs. A declaration remains nullable;
reads on a proven path use `T`. Assigning a local or parameter invalidates its
proof. Fields may be tested for presence but are not narrowed because aliases
and calls can mutate them. Copy a field to a local for stable refinement.

## Safe fields, calls, and meta queries

`receiver?.Field` evaluates the receiver once. It returns `null` on absence and
reads the field on presence. A non-null `T` result becomes `T?`; an existing
`T?` result stays nullable.

```cloth
string? name = user?.Name;
int32? age = user?.Age;
Point? position = user?.Position;
```

The same operator safely invokes declared instance functions:

```cloth
int32? age = user?.GetAge();
Point? moved = point?.Moved(1, 2);
logger?.Flush();
```

Arguments are not evaluated when the receiver is absent, and a skipped
throwing call cannot throw. A safe `void` call produces `void`. Static
functions, constructors, and unresolved members cannot be called safely.

Use `receiver?::query` for safe non-callable meta queries such as `length`,
`byteLength`, `isEmpty`, and `typeName`. It yields a nullable query result.
Callable meta operations such as `parse`, `slice`, `wrap`, and `sat` do not
support `?::`. Safe indexing and slicing are also unsupported; narrow or assert
the receiver first.

## Coalescing, assertion, and equality

`left ?? fallback` evaluates its left operand once and evaluates the fallback
only on absence. A compatible non-null fallback produces `T`; a nullable
fallback produces `T?`. The operator associates to the right.

Postfix `value!` asserts presence and returns the `T` payload. It terminates
with `non-null assertion failed` on absence. A successful assertion also
narrows subsequent reads of a stable local or parameter.

Two nullable values with the same underlying type support `==` and `!=`. Two
absent values are equal, one absent and one present value are unequal, and two
present values use the underlying type's equality rule.

## Representation and verification

Nullable references retain their target-width pointer representation. Nullable
primitive, enum, and struct values use an inline `{tag, aligned payload}`
aggregate and are never boxed solely for nullability. Tag `0` is absent with a
zeroed payload; tag `1` is present; all other tags are invalid. Struct payload
reference offsets are shifted by the target-derived payload offset so the GC
traces only canonical live slots.

Nullable aggregate parameters use value-pointer passing and nullable aggregate
returns use indirect result storage. LLVM compares or converts a payload only
after validating and branching on the tag. Runtime ABI 9 validates nullable
value assertions. Artifact format 7 and compiler ABI 6 preserve the distinct
tagged layout and `_C6` native identities across packages and source-free
linking.

The representation is compiler-owned and is not a persistence or FFI format.

## Construction guarantee

Every non-static field of non-null reference type must be initialized by its
declaration or definitely assigned on every constructor exit. If no
constructor exists, each such field requires a declaration initializer.
Mutable nullable class fields default to `null`; primitive fields retain
zero-value initialization.

A field cannot be read before initialization. Until every required field is
initialized, `self` cannot escape and instance functions cannot be called.
The definite-initialization analysis is shared with final fields, but a mutable
non-null field may be reassigned after initialization while a final field must
be initialized exactly once.
