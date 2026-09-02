# Proposal: Stage 25 named value enums

Status: **implemented; Stage 25 completed 2026-09-02**.

Enums were selected and their contract and implementation start approved on
2026-09-02. This document records the approved design; the implemented source
contract is [enums](../enums.md) and verification is in the
[exit audit](../testing.md#stage-25-enum-exit-audit). The stage charter
and work order are in [the roadmap](../../ROADMAP.md#stage-25-named-value-enums)
and [work ledger](../../TODO.md#stage-25-named-value-enums).

## Purpose

An enum represents one value from a closed set of named cases. It is a nominal
value type, not an integer alias or an allocated class instance. This gives
Cloth typed states and choices while preserving its file identity, visibility,
import, output, and separate-compilation contracts.

## File kind and syntax

```text
// Status.co
enum {
  Pending,
  Running,
  Complete,
}
```

The filename supplies `Status`; the declaration does not repeat it. Existing
imports may precede the envelope. A file without an explicit kind remains a
class. The existing reserved `enum` keyword is used; no keyword is added.

Approved grammar additions:

```ebnf
explicit_file_type = explicit_file_class | explicit_interface | explicit_enum ;
explicit_enum     = "enum" "{" enum_case { "," enum_case } [ "," ] "}" ;
enum_case         = identifier ;
```

An enum has at least one case. Cases are comma-separated, with an optional
trailing comma. Empty enums, duplicate case names, explicit values, payloads,
modifiers, constructors, fields, functions, inheritance/conformance clauses,
and nested declarations are rejected. Members outside the envelope are also
invalid. This stage defines cases, not an enum object model.

Case access uses ordinary declared-member syntax:

```text
// Main.co, in the same source package as Status.co
static func Main() {
  Status current = Status.Pending;
  current = Status.Running;
  if (current == Status.Running) {
    println(current);  // Status.Running in the root source package.
  }
}
```

`Status.Running` is an immutable value of type `Status`, not a field load or
constructor call. It is not assignable. Bare cases are not injected into caller
scopes, and cases cannot be selected through a value (`current.Running`).
`Status::Running` is invalid: `::` remains the meta-query separator in
expressions, while cases are declarations.

## Identity, visibility, and imports

The existing two-pass model registers file identities and then case symbols
before checking consumers. Source discovery order cannot change enum identity.
Two enums remain distinct even if every case name and internal tag matches.

The file stem determines the enum type's visibility under Cloth's existing
capitalization rule. Every enum case is public, regardless of its first
character. Anyone who can access the enum type can access every declared case;
public cases do not make an otherwise inaccessible type visible.

Cases describe the enum's complete set of possible values, not hidden object
implementation details. They are therefore an explicit exception to member
capitalization-based visibility. There are no private cases or case access
modifiers. Lowercase and underscore-prefixed case names remain public.
Identifiers remain case-sensitive: `Pending` and `pending`, if both declared,
are distinct cases, not aliases. Naming conventions do not control access.

Existing same-package lookup, explicit imports, aliases, wildcard imports, and
ambiguity diagnostics apply to the enum type. For example:

```text
import workflow::Status as JobStatus;
```

Consumers write `JobStatus.Running`. A wildcard imports public file types, not
their cases. Neither aliases nor display names replace canonical package,
version, namespace, file-stem, and enum-kind identity.

## Values and permitted operations

- Assignment, argument passing, returns, and copying require the same canonical
  enum type. `var` infers that type from a case or enum-valued expression.
- `==` and `!=` compare two values of the same enum and return `bool`.
- Overload selection treats enum types as distinct exact matches. Cases do not
  participate in contextual numeric literal typing or numeric widening.
- Enum fields, parameters, locals, and array elements are supported. Arrays
  remain invariant, and existing indexing and `for (var item in values)` work.
  Mixed-enum or enum/integer array literals are invalid.
- `final` keeps its ordinary binding semantics; enum cases themselves are
  always immutable, while an enum-typed variable may be reassigned.

There is no implicit or explicit enum/integer conversion, arithmetic, ordering,
bitwise operation, prefix/postfix update, truthiness, or enum construction call.
`Status(0)`, `uint32(current)`, and `if (current)` are invalid. Existing `if`
and loop conditions can use enum equality; this stage adds no `switch`, match,
or exhaustiveness machinery.

Enums do not widen to `object`, participate in reference `is`/`as`, or acquire
boxing. `Status?` is invalid until nullable value types have their own contract.
`Status[]?` remains a nullable array reference, not a nullable enum element.
Enum methods, interface conformance, and inheritance are outside this stage.

## Initialization and valid values

There is no implicit first-case or zero default. Choosing a program state must
be explicit; reordering declarations must not silently change default behavior.

- Enum locals require declaration initializers, including classical `for`
  locals. This does not introduce general local definite-assignment analysis.
- Instance fields need a declaration initializer or a direct assignment on
  every constructor exit. Extend the existing field-initialization analysis to
  prevent reads, instance calls, and `self` escape before required enum fields
  are initialized. Existing `final` single-assignment rules still apply.
- A class with an uninitialized enum field and no constructor is rejected;
  this does not synthesize a constructor.
- `static final Status Initial = Status.Pending;` is permitted as a compile-time
  case constant. Optional parentheses are permitted. Referencing another static
  field, evaluating a function, or accepting an arbitrary constant expression is
  not part of this extension. Mutable static fields remain unsupported.
- Array literals initialize every element. Existing empty/null-only literal
  restrictions remain; no default-filled array construction is introduced.

Valid values originate from cases or from copying already-valid enum values.
Zeroed allocation bytes are not observable enum initialization. No source
operation manufactures an unnamed value.

## Printing and meta queries

`print(value)` and `println(value)` accept any enum through typed core output,
without boxing. The representation is `qualified.EnumName.CaseName`; a root
`Status.Running` prints exactly that text. An import alias does not change it.
Printing evaluates its operand once and retains ordinary intrinsic shadowing
and newline behavior. Printed names are diagnostic text, not a wire format.

`value::typeName` returns the qualified source type name, such as `Status` or
`workflow.Status`, independent of the case. The operand is evaluated once,
including its side effects. This extends the meta query to enum values without
adding a managed object header. Type-level reflection, `::name`, `::values`,
case counts, ordinal queries, and string conversion syntax are deferred.

## Representation and compiler boundaries

Each case receives a unique unsigned 32-bit tag in source declaration order,
starting at zero. Tags are internal representation, not source integers or a
stable persistence contract. More cases than the representation or documented
compiler resource limits permit are diagnosed without count overflow. Changing
the case list changes package metadata and invalidates dependent artifacts.

Enum storage has the selected target's `uint32` size and alignment. Parameters
and results use the corresponding scalar ABI representation. Enum values have
no managed header, references, allocation entry, virtual table, or GC roots;
enum fields/elements must not be recorded as managed-reference slots. Arrays
containing enums are still ordinary managed arrays.

AST and semantic records retain the enum kind, ordered cases, source ranges,
type visibility, and canonical owner. Case symbols are always public rather
than inferring visibility from their spelling. HIR and MIR retain nominal enum
types and verified case constants; enums must not become ordinary integers
before type and operation verification. Verifiers reject invalid tags, mismatched owners,
and numeric operations on enum values. LLVM may then use integer constants,
copies, and equality. Output lowering must use bounded case-name selection and
trap on an invalid tag rather than perform an unchecked table access.

General constant folding is not required. Direct case constants and supported
static enum initializers are declaration semantics, not an optimizer stage.

## Separate compilation and compatibility

`clothc` owns enum identity, metadata, and validation. Shuttle continues to pass
opaque artifacts and receipts; this feature requires no manifest version or
second build protocol.

Imported views must preserve the canonical enum kind, type visibility, complete
ordered case set, case identities/names/tags, scalar layout, and static enum
constants. All cases of an accessible imported enum participate in public lookup,
including lowercase and underscore-prefixed names. If a shared symbol record
stores case visibility, it must be public; readers reject private-case records.
A consumer must type-check and print dependency enum values without reading
dependency source or reconstructing a synthetic class AST.

Artifact validation must reject duplicate names/tags, invalid tag ranges or
ordering, wrong owners, enum/reference confusion, invalid constants, incorrect
storage/reference maps, and missing or incompatible dependency identities.
Records remain bounded and canonically ordered. Native metadata has explicit
ownership; separate builds must not duplicate externally defined tables or
leave unresolved output helpers.

Before serialization changes land, review and freeze the enum record schema,
canonical kind/member tags, native metadata ownership, and required artifact
format/compiler-ABI revisions. The frozen format-1 schema is not silently
extended. Old artifacts must fail compatibility checks, not be reinterpreted;
unchanged runtime layouts do not by themselves require a runtime-ABI bump.
The decisions below record that review. Existing exact compiler/dependency identities and
Shuttle's conservative reuse rules remain in force.

### Stage 25 artifact decisions

Artifact format 2 adds an `enum_cases` array to file declarations (empty for
classes/interfaces). Each case has canonical `id`, `name`, `tag` (a canonical
decimal string), and `location`. Cases are public by construction, carry the
`enum-case` member identity domain, and are ordered by tag. A nominal `enum`
kind distinguishes enum types from classes and interfaces. Static enum
constants use literal kind `enum` with a decimal tag and the field's nominal
type. The case limit is 65,536 per enum, shared by source and artifact readers.

Compiler ABI revision 3 uses `_C3` canonical names, retaining the existing
length-prefixed encoding and adding the enum nominal/member domains. Runtime
ABI revision 1 and process protocol v2 remain unchanged. Format-1 artifacts
are rejected, not upgraded. Enum output tables/helpers are module-private and
derived from verified declarations, so consumers need no new external runtime
symbols or enum heap descriptors. The shared file ABI record is empty for an
enum (zero class size/header, alignment one, no members or descriptor linkage);
the enum type record owns its four-byte scalar layout.

The exact current schema is [artifact schema v2](../artifact_schema_v2.md);
the source-facing contract is [enums](../enums.md).

## Verification and completion

The staged work must cover:

- parser recovery for invalid enum bodies and deterministic AST/HIR output;
- nominal type separation, enum-type visibility, public access to uppercase,
  lowercase, and underscore-prefixed cases, case-sensitive lookup, import aliases
  and ambiguities, unsupported operations, and initialization across
  branches/early returns;
- overloads, static constants, class fields, constructor flow, arrays, iteration,
  copying, equality, and exactly-once output/meta evaluation;
- scalar ABI and GC layouts, enum-valued class/interface call signatures, and
  negative HIR/MIR/ABI/artifact verification;
- artifact round trips, malformed records (including private-case metadata),
  same-stem types from different packages, and public lookup of every case
  spelling in imported enums with dependency sources unavailable;
- byte-identical serial/parallel artifacts, dependency invalidation after case
  edits, and whole-project/separate native execution equivalence; and
- development, sanitizer, affected cross-tool, and native exit suites.

Implementation updates the owning grammar, semantic, flow/MIR, output, ABI,
identity, imported-view, artifact, and testing documents. README changes are
limited to user-facing examples if useful, not a new stage history. Deliberate
deferrals remain in `TODO.md`; structs and other backlog features do not enter
Stage 25 implicitly.
