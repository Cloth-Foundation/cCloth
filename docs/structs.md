# Cloth value structs

Structs are implemented through source checking, MIR, target ABI, native LLVM
lowering, precise GC, and source-free package artifacts. They are inline values,
not separately allocated GC objects. The compiler handles their storage and
traces managed references contained in them; programmers manage no lifetimes.

The [source contract](proposals/stage_26_structs.md) and
[aggregate ABI contract](proposals/stage_26_aggregate_abi.md) record the approved
design. The [Stage 26 exit audit](testing.md#stage-264-struct-exit-audit) is complete.

## Declaration and initialization

```cloth
// Point.co
struct {
  int32 X;
  int32 Y;

  Point(int32 x, int32 y) {
    X = x;
    Y = y;
  }

  func Moved(int32 dx, int32 dy): Point {
    return Point(X + dx, Y + dy);
  }
}
```

The filename supplies the nominal type name. Imports may precede the envelope;
declarations cannot follow it. File, field, function, and constructor visibility
use normal capitalization rules. `Point`, `point`, and `_Point` retain the
existing public/private constructor spellings; callers use `Point(...)`.

Every instance field, including primitive fields, must be initialized by its
declaration or a direct assignment on every constructor exit. Embedded struct
fields must receive a complete value before their subfields can be used. Reads,
copies/escapes of `self`, and instance calls before required initialization are
diagnosed. Final fields additionally require exactly one initialization.

Struct locals require initializers. Struct-valued class fields also require
initialization; primitive class-field defaults are unchanged. There are no
default struct values or synthesized constructors. Empty structs are permitted
but require a declared constructor to create a value.

## Values and storage

Assignments, arguments, returns, array reads, and iteration bindings copy values
with exact nominal type identity. Inline struct fields copy recursively; managed
references copy shallowly. Reading a value invokes no user code or constructor.

Only writable storage paths can be changed. `points[0].X++` targets the stored
element, while `MakePoint().X++` is invalid. A final struct binding protects all
inline fields. That protection stops at a managed reference: fields of an object
or elements of an array reached through a final struct remain mutable, but its
reference fields cannot be replaced.

Instance functions receive a read-only value snapshot before explicit arguments
are evaluated. They may read or return `self`, mutate local/parameter copies,
perform I/O, and mutate referenced objects. They cannot modify inline receiver
fields. Constructors alone receive writable, incomplete `self`; replacing
`self` wholesale is invalid. All struct instance calls are direct.

## Operations and restrictions

`==` and `!=` require identical struct types. Their contract is fieldwise equality
in declaration order, recursively for struct fields, using each field type's
existing equality. Padding and static fields do not participate.

`print`/`println` accept struct values without boxing; their output contract is
`<qualified.TypeName>`. `value::typeName` returns that qualified name. Both
evaluate their operand once. Equality includes private fields, compares
strings by contents and other references by identity, and preserves NaN behavior.

Arrays are invariant. `Point[]?` is a nullable array reference; `Point?` is not a
supported nullable value. Structs do not widen to `object`, participate in `is`/
`as`, provide truthiness or arithmetic, inherit, implement interfaces, or support
virtual/abstract/final-override functions. Static fields retain the existing
scalar-literal/enum-case `static final` contract, not aggregate constants.

Inline field cycles are rejected with the participating field path. Class and
array references break layout dependencies; function signatures do not add them.

## Frontend boundary

```sh
clothc --check --source-root=src src/Main.co
```

This runs source discovery, parsing, semantic checking, typed HIR verification,
and control-flow analysis. On success it prints the typed HIR summary; diagnostics
go to stderr. It emits no artifacts and cannot be combined with `--build` or
`--emit-llvm`. Both target selections use the same source-level value rules.

`Compilation::analyze_frontend` exposes the same result to compiler clients.
HIR expressions distinguish values, writable locations, and read-only locations;
member/index nodes retain storage paths. Ordinary struct callables/calls carry a
read-only-value receiver mode; constructors carry a construction mode. Bindings
and value-consuming operations copy struct locations; lowering
captures these values at their evaluation point rather than sharing their storage.

## Native and package representation

Use `clothc --build=program --source-root=src src/Main.co` (add `.exe` on Windows),
or `shuttle run` for a project. `--emit-llvm=program.ll` supports both target
layouts; native execution currently targets x86-64.

Fields are aligned in declaration order, including private fields and nested
values. Empty structs occupy one byte. Structs have no object header or heap
descriptor. Struct array elements use the same layout.

Explicit struct arguments receive independent writable copies. Instance methods
receive read-only snapshots captured before arguments. Struct returns use fresh
caller-owned result storage. Nested writes capture their owner and index before
the RHS; compound updates load the current field after evaluating the RHS.

Compatibility is artifact format **4**, compiler ABI **4**, runtime ABI **3**.
Process protocol **2**, receipt schema **1**, and manifest schema **1** are
unchanged. Rebuild old artifacts. See [data layout and ABI](data_layout_and_abi.md)
and [artifact schema v5](artifact_schema_v5.md) for layouts, maps, signatures, and
bounded-validation limits.
