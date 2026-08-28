# Cloth Stage 12.3.5 nullability contract

Cloth reference types are non-null by default. A trailing `?` admits the
`null` value and creates a distinct semantic type:

```cloth
User current;
User? selected = null;
```

`?` applies to the type immediately to its left. Array and element nullability
are therefore independent:

```cloth
User[] values;       // non-null array of non-null User values
User?[] values;      // non-null array whose elements may be null
User[]? values;      // nullable array of non-null User values
User?[]? values;     // nullable array whose elements may be null
```

Only references can be nullable. `string`, file classes, and arrays are
reference types; primitives and `void` are not. Forms such as `int32?` and
`void?` are diagnosed.

String properties return value types. Consequently, `value?.Length`,
`value?.ByteLength`, and `value?.IsEmpty` await nullable value types. Narrow a
`string?` through a presence/null check or use `value!` before querying them.

## Compatibility

Assignment compatibility is directional:

- `T` is assignable to `T` and `T?`.
- `null` is assignable only to `T?`.
- `T?` is not assignable to `T`.
- Different underlying types remain incompatible.

The same rules apply to initializers, assignments, arguments, returns, array
elements, and explicit `for` bindings. Overloads cannot differ only by
nullability because nullable qualifiers are erased by the callable ABI.

Array literals containing both non-null references and `null` infer a nullable
element type. `[user, null]` therefore has type `User?[]`. Empty and null-only
literals still require contextual literal typing and remain invalid.

## Use and narrowing

A nullable local or parameter can be narrowed after a direct comparison with
`null`. `value != null` proves `value` non-null on the true path;
`value == null` proves it on the false path. Reversed operands, parentheses,
logical negation, and short-circuit `&&` and `||` compose the same facts. This
supports both nested branches and guard clauses:

```cloth
func Name(User? value): string {
  if (value == null) { return "unknown"; }
  return value.Name;
}
```

The declaration remains `T?`; only reads on a proven path have type `T`.
Assigning the binding invalidates the proof. Fields are deliberately not
narrowed because an alias or instance call may mutate them without a local
assignment. Copy a field to a local when a stable refinement is needed.

## Presence and null operators

A nullable reference is a valid `if` or `while` condition. It is true when the
reference is non-null. Prefix `!` tests for null, and nullable operands in
`&&` and `||` use the same presence rule. A non-null reference condition is
rejected as always true rather than silently accepted:

```cloth
if (user) { println(user.Name); }
if (!user) { println("missing"); }
if (user && enabled) { println(user.Name); }
```

Presence tests narrow stable locals and parameters on the appropriate path.
Fields may be tested for presence but are not narrowed.

Safe access evaluates a nullable receiver once. It loads the reference-valued
field only when the receiver is non-null and otherwise produces `null`:

```cloth
string? name = user?.Name;
```

If the field has type `T`, the result is `T?`; if it already has type `T?`, the
result remains `T?`. Safe access to primitive fields awaits nullable value
types. Safe function calls are also deferred; narrow the receiver first.

The null-coalescing operator evaluates its left operand once and evaluates the
fallback only when that value is null:

```cloth
string display = user?.Name ?? "Unknown";
```

`left ?? fallback` requires a `T?` left operand. It produces `T` for a `T`
fallback, or `T?` for a compatible nullable fallback. The operator is
right-associative.

Postfix `value!` asserts that a `T?` value is present, returning `T`. It
evaluates the operand once and traps with `non-null assertion failed` when the
value is null. A successful assertion of a stable local or parameter also
establishes the same flow fact for subsequent reads.

Without a proof, a nullable value cannot be used for ordinary member access,
indexing, or iteration. HIR records narrowed reads with the underlying
`TypeId`. MIR uses
explicit conversions for both `T` or `null` to `T?` and proof-backed `T?` to
`T`. Presence tests, safe access, coalescing, and assertions retain explicit
MIR operations or control flow. ABI lowering erases nullable conversions: `T`
and `T?` use the same opaque reference layout and type encoding. This keeps
nullability a static contract without changing pointers or mangled symbols.

## Construction guarantee

Every non-static field of non-null reference type must be initialized by its
declaration or definitely assigned on every exit path of every constructor. If
no constructor exists, each such field requires a declaration initializer.
Nullable reference fields default to `null`; primitive fields retain their
zero-value initialization.

Constructor initialization uses a direct assignment statement to the current
instance, such as `Name = name;` or `self.Name = name;`. Branches establish
initialization only when every reachable path assigns the field. An assignment
inside a loop does not establish initialization after the loop, and an early
`return;` is checked as a constructor exit.

A field cannot be read before it is initialized. Until all non-null reference
fields are initialized, `self` cannot be used as a value and an instance
function cannot be called on the object. Direct field initialization remains
valid during that interval. These restrictions prevent a partially initialized
object from escaping or being observed through an instance function.

The definite-initialization analysis is shared with final fields, but the
contracts remain distinct: a mutable non-null field may be assigned again after
initialization, while a final field must be initialized exactly once.
