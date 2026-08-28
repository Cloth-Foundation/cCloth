# Cloth semantic analysis through Stage 16.5

Semantic analysis binds parsed syntax across a closed compilation graph, checks
the implemented language rules, and lowers valid and recovered syntax to a
typed, target-independent high-level intermediate representation (HIR).

## Compilation model

Each source contributes one implicit file class. The graph includes explicit
entry files, imported files, and project package siblings. All file classes are
registered before member signatures are resolved, and all member signatures
are registered before any initializer or body is checked. Forward references,
same-package references, and import cycles therefore have deterministic meaning.

After imports are bound, Stage 16.1 resolves every optional file-class base and
stores its `FileId` in `FileSemantics::base_file`. A base must be a visible file
class. Qualified-name-ordered graph traversal rejects self-inheritance and
indirect cycles without depending on discovery order. Stage 16.2 carries the
validated edge into HIR and MIR for ABI layout. Stage 16.3 binds every explicit
derived-constructor initializer to one constructor of the direct base. Stage
16.4 uses the same edge for inherited lookup, assignment compatibility, and
checked type operations. Stage 16.5 processes file classes base-first after all
signatures are registered, validates explicit overrides, and assigns stable
virtual slots.

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
- `string`
- `object`, the universal non-null managed-reference type
- `void`, plus internal error and null types
- one named reference type for each valid file class
- one canonical array reference type for each used element type
- one canonical nullable wrapper for each used nullable reference type

`int`, `uint`, and `float` are target-independent aliases of `int32`, `uint32`,
and `float32` respectively. Integer and floating literals have `int32` and
`float64` type respectively. General implicit numeric conversions are not
implemented. References are non-null by default. `T?` is a distinct nullable
wrapper, `T` widens to `T?`, and `null` is assignable only to `T?`. Nullable
qualifiers are invalid on primitive and void types.

File-class, string, and array references widen to `object`, and those nullable
forms widen to `object?`. The conversion is not available to primitives and
does not imply boxing. Arrays remain invariant: `User[]` does not widen to
`object[]`.

A file-class reference also widens to any direct or transitive base class.
Non-null-to-nullable and nullable-to-nullable forms compose with that
conversion. Base-to-derived and unrelated file-class assignments remain
invalid. Since the complete base is a prefix of the derived object, widening
does not change the runtime pointer.

`string` is a managed reference with immutable UTF-8 value semantics. String
literals are decoded and validated as UTF-8 during semantic analysis. Exact
`string + string` produces `string`; `==` and `!=` produce `bool` and use
content equality. `::length` and `::byteLength` are `int32` meta queries, and
`::isEmpty` is a `bool` meta query. Meta names are case-sensitive and bypass
ordinary member lookup and visibility. Nullable strings must be narrowed before
meta access.

An omitted function return annotation and explicit `: void` resolve to one
canonical type. Void has no values or storage: it is rejected for fields,
parameters, locals, arrays, and iteration bindings. Void calls are valid only
where their result is not consumed. Void functions may fall through or use
`return;`; value-returning functions retain complete-return requirements.

An array literal starts from its first non-null, non-error element. Different
managed-reference element types join at `object`; nullable references or
`null` make the joined element nullable before every element is checked.
Empty and null-only literals are rejected until contextual literal typing is
implemented. Index operands must be `int32`. Index expressions are mutable
locations, and `::length` is a read-only `int32` meta query.

The error type is a recovery value. It is compatible with every type solely to
prevent one failure from producing unrelated diagnostics.

`final` is stored on semantic symbols rather than semantic types. It prevents
rebinding fields, parameters, locals, and iteration variables without changing
the underlying value type. Final locals require initializers; `var` locals
infer the initializer's exact canonical type and reject missing, null-only, or
void initializers.

Final fields may use declaration initializers or direct assignments in their
defining constructors. Constructor analysis tracks definite initialization
through branches and early returns, rejects repeated or loop-based writes, and
rejects reads before initialization. Every constructor must initialize each
otherwise-uninitialized final field exactly once.

Every declared constructor in a derived file class must explicitly initialize
its direct base with `Derived(...): Base(...)`. A root constructor cannot have
that clause, the named type must be the direct base, and the arguments must
select one base constructor using normal overload rules. The initializer
is checked after parameter binding and before the constructor body. Because the
base subobject is not initialized yet, its expressions cannot use `self`, read
instance fields, or call unqualified instance functions. Cloth does not
synthesize constructors or silently choose a zero-argument base constructor.

The same flow analysis requires every non-null reference field to be
initialized by its declaration or on every constructor exit. Mutable non-null
fields may be reassigned after initialization. Reads before initialization are
rejected, loop-only writes do not establish post-loop initialization, and
constructor initialization must be a direct assignment to the current
instance. Until all non-null fields are initialized, `self` cannot be used as a
value and instance functions cannot be called on the current object. Nullable
reference and primitive fields retain their default initialization.

`static` is also stored on member symbols rather than types. Static functions
have no implicit `self` scope entry. Unqualified instance fields and functions
therefore fail in static bodies, as do instance-qualified static accesses and
file-class-qualified instance accesses. Static scalar fields must be final and
literal-initialized; they are excluded from instance final-field analysis.

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
primitive, `object`, and `null`; file classes and arrays use object widening,
and `println()` is a separate
zero-argument intrinsic. Locals and members retain normal precedence over core
names, so a source declaration can shadow either overload set. Exact parameter
matches take precedence over non-null-to-nullable widening. Callables cannot
overload solely on nullability because the ABI erases the qualifier.
Public static functions and fields can be qualified by a file-class name, such
as `Repository.Find(id)`. Instance members require an object.

File-class member lookup walks from the receiver's static type toward the root.
The nearest class declaring a name supplies the declaration set and hides
farther ancestors. Public declarations are inherited; private declarations are
visible only while analyzing their owning file class. Constructors are not
inherited. The selected public instance signature supplies a virtual slot, so a
base-typed receiver invokes the most-derived compatible implementation.

Every public instance function receives a virtual slot. A derived declaration
with the same name and canonical parameter types must use `override`, retain
the exact return type, and replace the inherited implementation in that slot.
New overloads append slots in declaration order. Private and static functions
remain direct and reject `override`. Calls in field initializers and constructor
bodies are direct only when their receiver is the object under construction,
so partially initialized derived state cannot be reached. Calls on other
receivers and ordinary function-body calls dispatch virtually.

The checker currently validates:

- field and local initializers
- mutable assignment targets and assigned values
- unary and binary operator operand types
- boolean `if` and `while` conditions
- `break` and `continue` placement inside loops
- inferred or explicitly typed array iteration declarations
- final binding assignment and field definite initialization
- static ownership, access form, and static `Main` validation
- member access and visibility
- array literal inference, indexing, assignment, and `::length`
- object and base widening, `::typeName`, hierarchy-aware checked `is`, and
  nullable `as`
- single-inheritance base binding, visibility, cycle validation, and explicit
  base-constructor selection, inherited member lookup, and override contracts
- exact overload and constructor selection
- return value presence and type compatibility

Nullable locals and parameters are narrowed to their underlying reference type
on paths proven by direct `null` equality or inequality checks or nullable
presence conditions. Parentheses, `!`, `&&`, and `||` compose true- and
false-path facts, including guard clauses. Assignments invalidate a fact, and
branch joins retain it only when every fallthrough path does. Fields are
excluded until member effects and aliasing have a stronger contract. Without a
proof, nullable values cannot be directly dereferenced, indexed, or iterated.
Safe reference-field access produces a nullable result, null coalescing checks
a compatible lazy fallback, and postfix assertion produces the underlying type
with a runtime null guard.

Overload matching prefers an exact canonical signature. If none exists, a
unique candidate accepting the implemented implicit reference conversions is
selected; multiple compatible candidates are ambiguous.
User-defined conversions, numeric promotions, base-qualified calls, abstract
members, traits, generics, primitive boxing, first-class function values, and
implicit default constructors are deferred. Checked array casts are also
deferred until arrays carry reified element-type metadata.
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

A derived `HirCallable` also carries its selected base-constructor `SymbolId`
and typed argument expressions. This preserves the semantic decision without
introducing target layout or a calling convention into HIR.

HIR is intentionally independent of target layout, ABI, object representation,
runtime calling conventions, and garbage-collector strategy. Those decisions
belong after semantic analysis.
