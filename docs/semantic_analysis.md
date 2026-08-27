# Cloth Stage 2.0 semantic analysis

Semantic analysis binds parsed syntax across a closed compilation graph, checks
the implemented language rules, and lowers valid and recovered syntax to a
typed, target-independent high-level intermediate representation (HIR).

## Compilation model

Each source contributes one implicit file class. The graph includes explicit
entry files, imported files, and project package siblings. All file classes are
registered before member signatures are resolved, and all member signatures
are registered before any initializer or body is checked. Forward references,
same-package references, and import cycles therefore have deterministic meaning.

`Compilation` owns its source files, immutable token streams, and parse results.
This keeps token lexemes, syntax names, and source-range file names valid
through semantic analysis and HIR consumption.

Project compilations sort sources by qualified identity before allocating
stable `FileId`, `TypeId`, and `SymbolId` handles. Explicit standalone
compilations preserve input order. Source files whose qualified identities
differ only by ASCII case are rejected on every host.

## Types

The core type table contains:

- `bool`, `char`, and `byte`
- `int8`, `int16`, `int32`, and `int64`
- `uint8`, `uint16`, `uint32`, and `uint64`
- `float32` and `float64`
- `String`
- `void`, plus internal error and null types
- one named reference type for each valid file class
- one canonical array reference type for each used element type

`int`, `uint`, and `float` are target-independent aliases of `int32`, `uint32`,
and `float32` respectively.
Integer and floating literals have `int32` and `float64` type respectively.
General implicit numeric conversions are not implemented. `null` is assignable
to `String`, file-class, and array reference types, but not to value types.

An omitted function return annotation and explicit `: void` resolve to one
canonical type. Void has no values or storage: it is rejected for fields,
parameters, locals, arrays, and iteration bindings. Void calls are valid only
where their result is not consumed. Void functions may fall through or use
`return;`; value-returning functions retain complete-return requirements.

An array literal infers its element type from the first non-null, non-error
element, then requires every element to be assignable to that exact type.
Empty and null-only literals are rejected until contextual literal typing is
implemented. Index operands must be `int32`. Index expressions are mutable
locations, and `Length` is a read-only `int32` value.

The error type is a recovery value. It is compatible with every type solely to
prevent one failure from producing unrelated diagnostics.

## Binding and checking

Names are resolved from the innermost lexical scope outward, followed by the
current file-class members, current-package file classes, explicit imports,
wildcard imports, and the core scope. Parameters and locals may be shadowed by
nested blocks but may not be redeclared in the same scope. `self` is an
intrinsic immutable reference to the current file-class instance.

Capitalization-based visibility is enforced for both named types and members.
Private declarations remain accessible inside their defining file class.
Imports are file-scoped and non-transitive. Explicit aliases disambiguate
otherwise conflicting file-class names. Wildcards expose only public direct
members of one package, and ambiguous wildcard names are diagnosed.

The core scope provides typed `print` and `println` overloads for every
primitive, each file-class type, and `null`; `println()` is a separate
zero-argument intrinsic. Locals and members retain normal precedence over core
names, so a source declaration can shadow either overload set. Exact parameter
matches take precedence over nullable-reference conversions, keeping
`print(null)` unambiguous as file-class overloads are added.
Public functions can be qualified by a file-class name, such as
`Repository.Find(id)`. Fields require an instance.

The checker currently validates:

- field and local initializers
- mutable assignment targets and assigned values
- unary and binary operator operand types
- boolean `if` and `while` conditions
- `break` and `continue` placement inside loops
- inferred or explicitly typed array iteration declarations
- member access and visibility
- homogeneous array literals, indexing, assignment, and `Length`
- exact overload and constructor selection
- return value presence and type compatibility

Overload matching is exact after the portable aliases are canonicalized.
User-defined conversions, numeric promotions, inheritance, traits, generics,
first-class function values, and implicit default constructors are deferred.
Complete return-path and reachability checks are performed by the Stage 3.0
control-flow analysis after HIR verification.

A `for` iterable is checked before its loop binding enters scope. Arrays expose
their canonical element type to the binding. `var` adopts that type; an
explicit declaration uses ordinary assignment compatibility. The binding is a
mutable local visible only in the loop body. Because an array may be empty, a
return from every iteration body does not by itself complete a function's
return paths.

## Typed HIR

HIR owns stable numeric handles and records a `TypeId` on every expression.
Names, member accesses, calls, constructors, parameters, and locals carry bound
`SymbolId` values where resolution succeeded. Invalid nodes remain representable
so tooling and later compiler stages can inspect recovered compilations.

HIR is intentionally independent of target layout, ABI, object representation,
runtime calling conventions, and garbage-collector strategy. Those decisions
belong after semantic analysis.
