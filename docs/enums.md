# Named value enums

An enum is a nominal value type with a closed set of named cases. Its `.co`
filename supplies its name, just as it does for classes and interfaces:

```cloth
// Status.co
enum {
  Pending,
  running,
  _Done,
}
```

Cases are comma-separated, with an optional trailing comma. An enum needs
between 1 and 65,536 distinct cases. Identifiers are case-sensitive and cannot
be keywords. Imports may precede the envelope; other declarations cannot occur
inside or after it.

## Access and values

All cases are public regardless of capitalization. The enum type itself still
gets its visibility from the filename. Accessing a public case does not bypass
an inaccessible type. Existing imports, aliases, wildcard type imports, and
ambiguity rules apply unchanged. Wildcards never import bare cases.

```cloth
Status current = Status.Pending;
current = Status.running;
if (current != Status._Done) {
  println(current);
}
```

`Status.running` is a constant of type `Status`, not a field or a construction
call. Cases are selected through the type using `.`, not through an instance
or the `::` meta separator. Case declarations cannot be assigned to.

Assignments, parameters, returns, overloads, fields, and arrays retain exact
nominal identity. `var` infers it. Only `==` and `!=` compare enum values, and
both operands must have the same enum type. Arrays support ordinary indexing,
mutation, and `for (var item in values)` iteration.

There are no enum/integer casts, implicit conversions, ordering, arithmetic,
bitwise operations, increment/decrement, truthiness, boxing, or reference
`is`/`as`. `Status?` is invalid; `Status[]?` is a nullable array reference.

## Initialization

There is no implicit first-case or zero default. Enum locals need declaration
initializers. An instance field needs a declaration initializer or a direct
assignment on every constructor exit. Reading the field, making an instance
call, or allowing `self` to escape before required fields are initialized is
rejected. `final` retains ordinary single-assignment rules.

```cloth
static final Status Initial = Status.Pending;
```

A static enum constant must directly name a case, optionally parenthesized.
Other static fields, calls, and general constant expressions do not qualify.

## Output and representation

`print` and `println` produce the qualified type and case name, for example
`Status.running` or `models.workflow.Status.running`. Aliases do not change
the output. `current::typeName` produces the qualified enum type name. Both
evaluate their value expression exactly once. These names are diagnostic text,
not a serialization format.

The compiler assigns internal `uint32` tags in declaration order, starting at
zero. Values occupy four bytes with the target's `uint32` alignment, have no
managed header, and contribute no GC reference slots. Enum arrays themselves
remain managed references. Tags are not stable persistence IDs.

HIR/MIR preserve the nominal type and validate constants before LLVM lowers
them to scalars. Printing uses module-private, bounds-checked case-name tables
and traps on invalid tags. No enum heap descriptor or new runtime ABI is added.

Source-free package consumers retain every case and static enum constant.
This requires [artifact format 2](artifact_schema_v2.md) and compiler ABI 3;
older artifacts must be rebuilt. Shuttle's process protocol remains version 2.

## Deliberate deferrals

Attached immutable per-case metadata, struct-backed metadata, runtime payload
variants, enum members/constructors, explicit discriminants, underlying types,
flags, matching/exhaustiveness, nullable values, and reflection are not part of
this contract. Metadata may eventually describe a case without becoming its
identity. These remain tracked in the [work ledger](../TODO.md).
