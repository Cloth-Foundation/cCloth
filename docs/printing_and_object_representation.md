# Cloth Stage 10.5 printing and object representation

Stage 10.5 completes deterministic scalar output and gives file-class objects
a stable default representation. Output remains a typed core operation rather
than parser syntax or a C-style variadic facility.

## Source contract

The core scope provides `print(T)` and `println(T)` for these types:

- `String`, `bool`, and `char`
- `byte`, every fixed-width signed and unsigned integer, and their aliases
- `float32`, `float64`, and the `float` alias
- every file-class type and the `null` literal

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
- the untyped `null` literal: `null`

A nullable reference must be narrowed before it can be passed to a non-null
typed output overload. Invalid Unicode scalar values trap. Windows native
output is placed in binary mode so the line-feed contract is byte-identical
across supported hosts.

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

`String` and arrays remain opaque runtime reference types rather than
file-class objects, but Stage 13.4 gives them the common managed header and
collector lifecycle. Array rendering, user-defined string conversion,
interpolation, and type-checked formatting are deferred. Cloth does not expose
C `printf` or an unchecked variadic formatting ABI.
