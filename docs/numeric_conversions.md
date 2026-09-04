# Numeric literals and conversions

Stage 20.1 defines contextual numeric literals and lossless primitive numeric
widening. These rules operate on values and are independent of target byte
order. Endianness belongs to storage layout, serialization, and native ABI
lowering rather than source-level assignment compatibility.

## Literal types

An integer literal defaults to `int32` and a decimal floating literal defaults
to `float64` when no numeric type is expected:

```cloth
var count = 10;   // int32
var ratio = 0.5;  // float64
```

A field, explicitly typed local, assignment, return, explicitly typed array
literal, or enclosing numeric expression supplies an expected type. A literal
adopts that integer or floating type only when its value is representable:

```cloth
int64 count = 10;
count = 20;
uint maximum = 4294967295;
float ratio = 0.5;
int64[] values = [1, 2, 3];
```

Integer ranges are checked with the selected signedness and width. The full
`uint64` range is available, negative literals are rejected for unsigned
targets, and unary minus may form the minimum value of a signed type. A decimal
literal selected as `float32` or `float64` is rounded once to that IEEE-754
format and rejected when the result is not finite.

Stage 20.2 applies the same representability rule during function, ordinary
constructor, and base-constructor overload resolution. A numeric literal
expression may contain parentheses and unary `+` or `-`. Resolution first
prefers a candidate whose complete parameter list exactly matches the literals'
default types and the other arguments' existing types. If no exact candidate
exists, literal-fit and ordinary widening candidates participate together; one
candidate must be uniquely compatible. Multiple compatible candidates are
ambiguous and declaration order never breaks the tie.

After selection, each numeric literal expression adopts its corresponding
parameter type. MIR therefore contains a literal of that type rather than a
default literal followed by a numeric conversion.

## Lossless widening

An implicit numeric conversion is allowed only when every source value has an
exact representation in the destination type:

- signed integers widen to larger signed integers;
- unsigned integers, including `byte`, widen to larger unsigned integers;
- an unsigned integer widens to a signed integer only when the signed type has
  a greater bit width;
- `float32` widens to `float64`.

Equal-width types do not implicitly convert merely because their ranges
overlap. Signed-to-unsigned, integer-to-floating, floating-to-integer, and all
narrowing conversions require the explicit syntax defined below.

Binary numeric operators are symmetric. When either operand can losslessly
widen to the other operand's type, both operands use that common type. Compound
assignment is directional because the result must remain compatible with its
target:

```cloth
int16 small = 10;
int32 wide = 20;
int32 sum = small + wide;  // small widens to int32.
wide += small;             // Valid.
small += wide;             // Error: would narrow int32 to int16.
```

MIR records each typed-value widening with `kWidenNumeric`. LLVM lowering uses
sign extension for signed integers, zero extension for unsigned integers, and
floating-point extension for `float32` to `float64`.

## Checked explicit conversion

Stage 20.3 uses target-type syntax for every explicit numeric conversion:

```cloth
int8 small = int8(value);
uint count = uint(signedCount);
float ratio = float(preciseRatio);
int32 whole = int32(decimalValue);
```

The syntax is a dedicated numeric-conversion expression. It is not an object
constructor and does not reuse nullable reference `as`. The operand is
evaluated exactly once. Identity conversions are valid and do not require a
MIR instruction.

Runtime values follow these rules:

- integer-to-integer conversion traps unless the mathematical value is in the
  destination range;
- floating-to-integer conversion truncates toward zero, then traps unless the
  result is in range; NaN and infinities therefore trap;
- integer-to-floating conversion uses IEEE-754 round-to-nearest,
  ties-to-even;
- `float64`-to-`float32` conversion rounds once, preserves NaN and infinities,
  and traps when a finite value overflows the finite `float32` range;
- floating underflow and ordinary precision loss are accepted because the
  conversion is explicit.

No checked conversion wraps, saturates, reinterprets bits, or converts a
numeric value to `bool`. Wrapping and saturation use the distinct integer-only
forms below.

A numeric literal expression is validated at compile time and adopts the
destination type directly. Invalid constants are diagnostics rather than
runtime traps:

```cloth
uint64 maximum = uint64(18446744073709551615);
int8 valid = int8(127);
int8 invalid = int8(128);  // Compile-time error.
```

HIR retains the explicit conversion. MIR uses `kCheckedNumeric` only for an
already typed runtime value; literal conversions lower directly as target-typed
constants. The LLVM backend emits the required range predicate before any
truncating conversion and terminates through the runtime failure path when the
predicate is false.

## Wrapping and saturating integer conversion

Stage 30 adds target-owned integer conversion modes without weakening checked
conversion:

```cloth
int8 wrapped = int8::wrap(300);   // 44
int8 limited = int8::sat(300);    // 127
uint8 residue = uint8::wrap(-1);  // 255
uint8 floor = uint8::sat(-1);     // 0
```

The target and operand must be non-nullable integer types. `int`, `uint`, and
`byte` keep their normal aliases and widths. The argument is analyzed and
evaluated exactly once without a target-type expectation.

`Target::wrap(value)` computes the least nonnegative residue modulo the target
width and interprets a signed target as two's-complement. `Target::sat(value)`
clamps the mathematical source value to the target's inclusive range. Identity
and in-range conversions are valid; neither form traps because of range.

Static scalar constants use the same conversion routine and store only
canonical target bits. HIR and MIR retain the explicit mode for runtime values.
MIR uses `kWrapInteger` or `kSaturateInteger`; verification requires exact
integer source/result types and a value result. LLVM wrapping uses truncation or
signed/unsigned extension. Saturation uses signed/unsigned comparisons and
selection before the required truncation or extension. Neither mode calls the
checked-conversion runtime helper or introduces another runtime symbol.
