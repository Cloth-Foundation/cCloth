# Integer binary data

Stage 21 defines portable bitwise operations and explicit byte-order access for
Cloth's fixed-width integer primitives. These operations never expose native
addresses, object layout, or host byte order.

## Integer operators

`byte`, `int8`, `int16`, `int32`, `int64`, `uint8`, `uint16`, `uint32`, and
`uint64` are integer operands. `int` and `uint` are aliases of `int32` and
`uint32`. `bool`, `char`, and floating-point values are not integer operands.

`&`, `|`, and `^` use the same common-type rule as arithmetic integer
operators: equal types remain equal, and one operand may widen only through an
existing lossless implicit numeric conversion. The result has that common
integer type. `~value` preserves the operand type and complements every bit in
its fixed-width representation.

For `value << count` and `value >> count`, the left operand determines the
result type and bit width. The count may have any integer type and is not used
to widen the value. A valid count is in `[0, bit_width)`. A syntactic integer
literal outside that interval is a compile-time error; every dynamic count is
checked before the shift and traps with `shift count is out of range` when
invalid.

Left shift discards bits shifted beyond the fixed width and introduces zero
bits on the right. Right shift is arithmetic for signed integers and logical
for unsigned integers. These rules are independent of the LLVM target and the
compiler host.

`&=`, `|=`, and `^=` require the right operand to be losslessly assignable to
the target integer type. `<<=` and `>>=` accept any integer count and preserve
the target type. Every compound target is evaluated exactly once.

## Byte-order meta operations

Encoding writes into an existing `byte[]` and returns `void`:

```cloth
value::writeLittleEndian(destination, offset);
value::writeBigEndian(destination, offset);
```

The receiver must be an integer. `destination` must be a non-null `byte[]`, and
`offset` must be `int32`. The operation writes exactly `bit_width / 8` bytes.
Signed integers are encoded from their fixed-width two's-complement bit pattern.

Decoding is a meta operation on `byte[]`:

```cloth
int32 value = bytes::readInt32LittleEndian(offset);
uint64 flags = bytes::readUint64BigEndian(offset);
```

The complete read names are `readByte`, `readInt8`, `readInt16`, `readInt32`,
`readInt64`, `readUint8`, `readUint16`, `readUint32`, and `readUint64`, each
followed by `LittleEndian` or `BigEndian`. Alias spellings such as `readInt` and
`readUint` are intentionally absent. Eight-bit little-endian and big-endian
operations have identical byte results but retain both spellings for a uniform
API.

Receivers and arguments are evaluated from left to right exactly once. A byte
range is valid when `offset >= 0` and the complete operation width fits within
the array. The runtime validates the entire range before reading or writing;
an invalid range traps with `integer byte range is out of bounds`, and an
invalid write changes no bytes.

No operation observes or selects native endianness. Floating-point bit access,
rotations, unsafe views, and general serialization remain outside Stage 21.
