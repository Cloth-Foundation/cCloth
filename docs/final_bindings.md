# Cloth Stage 12.1 final bindings

Stage 12.1 adds `final` as a single-assignment declaration contract. It does
not make an object or array immutable; it prevents the declared binding from
being assigned again.

For a [struct value](structs.md), final also protects its inline fields. Read-only
propagation stops at a class or array reference, preserving the existing mutable
referent contract. These rules are preserved through native aggregate lowering.

## Supported declarations

`final` may modify fields, parameters, locals, and `for` iteration bindings:

```cloth
final int32 Code;

Example(final int32 code) {
    Code = code;
}

func Read(final string name, int32[] values): void {
    final var copy = name;
    for (final var value in values) {
        println(value);
    }
}
```

An explicitly typed final local requires an initializer. `var` is now also
available for local inference and always requires an initializer, whether or
not it is final. Inference preserves the initializer's exact canonical type;
`null` alone cannot supply a type.

Final parameters and iteration bindings are initialized by the caller and the
iteration operation respectively. Assigning either binding again is invalid.

## Final fields

A final field is initialized either by its declaration or exactly once by each
constructor. Every constructor exit, including an explicit `return;`, must see
the field definitely initialized on every reachable branch.

Constructor initialization must be a direct assignment statement to the
current instance, using either `Field = value;` or `self.Field = value;`.
Initialization inside a loop is rejected because it cannot guarantee exactly
one write. Assigning a declaration-initialized field again is also rejected.

Field initializers execute in source declaration order. Reading a later final
field, or the field currently being initialized, is a read-before-initialization
error. A final field without a declaration initializer requires at least one
constructor.

## Mutability boundary

Final applies to the binding, not the referenced value:

```cloth
final int32[] values = [1, 2];
values[0] = 3;       // valid: the array object remains mutable
values = [4, 5];     // invalid: the final binding cannot be replaced
```

The qualifier is retained on semantic symbols and in diagnostic IR summaries.
It does not change type identity, overload selection, object layout, mangling,
or the LLVM representation. Stage 12.2 builds on that separation: `static`
controls instance ownership and callable ABI, while `final` continues to
control rebinding.
