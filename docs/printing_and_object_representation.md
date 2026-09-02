# Cloth Stage 10.5 printing and object representation

Stage 10.5 completes deterministic scalar output and gives file-class objects
a stable default representation. Output remains a typed core operation rather
than parser syntax or a C-style variadic facility.

## Source contract

The core scope provides `print(T)` and `println(T)` for these types:

- `string`, `bool`, and `char`
- `byte`, every fixed-width signed and unsigned integer, and their aliases
- `float32`, `float64`, and the `float` alias
- `object` and the `null` literal; file classes and arrays widen to `object`
- each enum type, without boxing

`print(value)` writes only the value. `println(value)` uses the same
representation and then writes one line-feed byte. `println()` writes one
line-feed byte. Neither function inserts spaces, accepts multiple values, or
interprets formatting placeholders. Locals and members retain lookup
precedence, so a source declaration named `print` or `println` shadows the
corresponding core overload set.

Values have these deterministic representations:

- strings: their bytes, unchanged
- booleans: `true` or `false`
- characters: the Unicode scalar encoded as UTF-8
- integers: base-10 digits, with a leading minus sign only when negative
- finite floats: the locale-independent shortest round-trippable decimal
- infinities and NaN: `inf`, `-inf`, and `nan`
- file-class objects: `<qualified.TypeName>`
- enum values: `qualified.TypeName.CaseName`
- arrays passed as `object`: `<Array>`
- the untyped `null` literal: `null`

A nullable reference must be narrowed before it can be passed to a non-null
typed output overload. Invalid Unicode scalar values trap. Windows native
output is placed in binary mode so the line-feed contract is byte-identical
across supported hosts.

## Enum output

`print` and `println` accept any enum and print `qualified.Type.Case`, preserving
the declared case spelling. Import aliases do not change it. Enum values are
not boxed. `value::typeName` returns the qualified nominal enum name, evaluating
the value once even when that name is statically known. Lowering uses private
bounds-checked name tables; an invalid tag traps instead of indexing outside
the table. See [enums](enums.md).

## Struct output checking

The frontend accepts `print`/`println` on structs without boxing and resolves
`value::typeName`. The approved output is `<qualified.TypeName>`; execution is
pending aggregate lowering. See the [struct contract](structs.md). No runtime
entry or descriptor is introduced by frontend checking.

## Object metadata

The first file-class header word points to an opaque runtime type descriptor.
The second stores opaque collector state owned exclusively by the runtime.
Stage 13.1 descriptors are immutable compiler-emitted globals containing the
qualified file-class name, verified object layout, object kind, and exact
reference-field offsets. Stage 13.3 allocation initializes both header words
before field initializers or constructor statements run.

Default object output reads only the descriptor. It never exposes an address,
field value, allocation order, or collector state. This keeps output stable if
a future garbage collector moves objects.

`string` and arrays remain opaque runtime reference types rather than
file-class objects, but Stage 13.4 gives them the common managed header and
collector lifecycle. Array rendering, user-defined string conversion,
interpolation, and type-checked formatting are deferred. Cloth does not expose
C `printf` or an unchecked variadic formatting ABI.

Stage 15 exposes stable type identity separately through `value::typeName`.
That query returns a string (`qualified.Type`, `string`, or erased `array`)
without changing default output or exposing an address.
