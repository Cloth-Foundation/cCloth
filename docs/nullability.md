# Cloth Stage 12.3.4 nullability contract

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

Only references can be nullable. `String`, file classes, and arrays are
reference types; primitives and `void` are not. Forms such as `int32?` and
`void?` are diagnosed.

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
func Name(User? value): String {
  if (value == null) { return "unknown"; }
  return value.Name;
}
```

The declaration remains `T?`; only reads on a proven path have type `T`.
Assigning the binding invalidates the proof. Fields are deliberately not
narrowed because an alias or instance call may mutate them without a local
assignment. Copy a field to a local when a stable refinement is needed.
Safe-navigation and forced-unwrapping syntax remain deferred.

Without a proof, a nullable value cannot be used for member access, indexing,
or iteration. HIR records narrowed reads with the underlying `TypeId`. MIR uses
explicit conversions for both `T` or `null` to `T?` and proof-backed `T?` to
`T`. ABI lowering erases both conversions: `T` and `T?` use the same opaque
reference layout and type encoding. This keeps nullability a static contract
without changing runtime pointers or mangled symbols.

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
