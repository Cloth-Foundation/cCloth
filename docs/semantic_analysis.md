# Cloth Stage 2.0 semantic analysis

Stage 2.0 binds parsed syntax across an explicitly supplied compilation set,
checks the implemented language rules, and lowers valid and recovered syntax to
a typed, target-independent high-level intermediate representation (HIR).

## Compilation model

Each input file contributes one implicit file class. All file classes are
registered before member signatures are resolved, and all member signatures are
registered before any initializer or body is checked. Forward references and
input declaration order therefore have the same meaning.

`Compilation` owns its source files, immutable token streams, and parse results.
This keeps token lexemes, syntax names, and source-range file names valid
through semantic analysis and HIR consumption.

Compilation input order defines stable `FileId`, `TypeId`, and `SymbolId`
allocation. Tables use ordered storage, and diagnostic traversal follows source
and declaration order. Source files whose stems differ only by ASCII case are
rejected on every host. Imports and directory-derived modules remain deferred;
the driver currently treats every command-line source as part of one compilation
set.

## Types

The core type table contains:

- `bool`, `char`, and `byte`
- `int8`, `int16`, `int32`, and `int64`
- `uint8`, `uint16`, `uint32`, and `uint64`
- `float32` and `float64`
- `String`
- internal error, no-value, and null types
- one named reference type for each valid file class

`int` is a target-independent alias of `int32`; `uint` is an alias of `uint32`.
Integer and floating literals have `int32` and `float64` type respectively.
General implicit numeric conversions are not implemented. `null` is assignable
to `String` and file-class reference types, but not to value types.

The error type is a recovery value. It is compatible with every type solely to
prevent one failure from producing unrelated diagnostics.

## Binding and checking

Names are resolved from the innermost lexical scope outward, followed by the
current file-class members and compilation file classes. Parameters and locals
may be shadowed by nested blocks but may not be redeclared in the same scope.
`self` is an intrinsic immutable reference to the current file-class instance.

Capitalization-based visibility is enforced for both named types and members.
Private declarations remain accessible inside their defining file class.

The core scope provides `print(String)`, `print(int32)`, and `print(bool)` as
typed intrinsic overloads. Locals and members retain normal precedence over
core names, so a source declaration can shadow `print`. Calls still use exact
parameter matching.
Public functions can be qualified by a file-class name, such as
`Repository.Find(id)`. Fields require an instance.

The checker currently validates:

- field and local initializers
- mutable assignment targets and assigned values
- unary and binary operator operand types
- boolean `if` and `while` conditions
- `break` and `continue` placement inside loops
- member access and visibility
- exact overload and constructor selection
- return value presence and type compatibility

Overload matching is exact after the `int` and `uint` aliases are canonicalized.
User-defined conversions, numeric promotions, inheritance, traits, generics,
first-class function values, and implicit default constructors are deferred.
Complete return-path and reachability checks are performed by the Stage 3.0
control-flow analysis after HIR verification.

## Typed HIR

HIR owns stable numeric handles and records a `TypeId` on every expression.
Names, member accesses, calls, constructors, parameters, and locals carry bound
`SymbolId` values where resolution succeeded. Invalid nodes remain representable
so tooling and later compiler stages can inspect recovered compilations.

HIR is intentionally independent of target layout, ABI, object representation,
runtime calling conventions, and garbage-collector strategy. Those decisions
belong after semantic analysis.
