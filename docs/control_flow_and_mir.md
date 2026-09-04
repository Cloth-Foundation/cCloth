# Cloth Stage 3.0 control flow and MIR

Stage 3.0 verifies typed HIR, performs callable-level control-flow analysis, and
lowers executable definitions to a target-independent mid-level intermediate
representation (MIR). MIR is the boundary between language semantics and future
target layout, ABI, optimization, and code generation.

[Struct values](structs.md) retain exact nominal types in MIR. Loads, argument
evaluation, returns, and receiver evaluation capture independent values. Storage
loads/stores carry a non-escaping recipe rooted in a local, parameter, construction
self, captured object, or captured array/index, followed by inline field projections.
Owner/index evaluation occurs before the RHS; a compound update reads its field
after the RHS. Storage owners remain live across every intervening safepoint.

MIR verification checks storage roots and projections, callable storage ownership,
read-only/final boundaries, receiver modes, complete construction, and value types.
Constructor dataflow rejects uninitialized reads/escapes and repeated final-field
initialization. Phi nodes cover every unique predecessor at block entry; aggregate
phis preserve simultaneous value assignment across loop edges.

## Verification

### Switch representation and control flow

Stage 27.2 retains each selector and scoped arm once in AST/HIR. HIR labels carry
canonical typed integer bits or nominal enum tags, source ranges, and optional
constant symbols. The verifier checks selector/category identity, bit widths,
case ownership, duplicates, coverage/default metadata, label/arm bounds, and
callable transfer contexts. Cyclic/shared callable blocks are rejected before
flow traversal.

Semantic non-null flow records break and continue joins in a nested control
context stack. Switches consume their breaks but not enclosing-loop continues;
return/continue paths do not join after the switch. Constructor initialization
uses the same structured target distinction. Return/reachability analysis
consumes switch breaks so they cannot make an enclosing infinite loop finite.
Unreachable statements remain type-checked without exporting flow facts.

Stage 27.3 lowers the captured selector to one typed `MirSwitchTerminator`.
Its case table is sorted by normalized unsigned bits and pairs exact typed
constants with targets. Every switch has an explicit default successor; enums
also have an invalid-tag successor, verified to be an empty `MirTrapTerminator`
block. An exhaustive enum without source default uses that trap as its default.
An integer without source default uses the join. No runtime label evaluation or
source-level comparison chain is introduced.

MIR verification rechecks selector definitions (including dominance), nominal
ownership, exact case types, bit/tag ranges, sorted uniqueness, targets, label
limits, and trap structure. `mir_successors` enumerates sorted unique targets,
including the invalid-tag path. Reachability, phi predecessors, struct constructor
initialization, and backend GC dataflow all use this shared enumeration.
The lowering transfer stack gives switches a break target but no continue target.

LLVM uses integer `switch`, guarding an unmatched enum tag against the verified
case count before entering a source default. Invalid tags call `llvm.trap`.
Successor blocks with phis receive one edge bridge per switch predecessor,
including shared case/default destinations. This preserves MIR's unique-block
phi rule despite LLVM's physical edges. See [LLVM lowering](llvm_backend.md).

### Existing representation checks

The HIR verifier checks stable-handle bounds and relationships among files,
declarations, blocks, statements, expressions, types, and symbols. Recovered
invalid nodes are valid compiler data; broken references are compiler-internal
errors.

The MIR verifier checks:

- file, type, symbol, value, and basic-block identities
- exactly one terminator per basic block
- unique and complete temporary-value definitions
- explicit value types at loads, stores, calls, conversions, phi nodes, branch
  conditions, and returns
- callable parameter-symbol mappings
- entry reachability and stored reachability flags

Verification failures are reported as internal compiler diagnostics and make the
compilation invalid.

## Control-flow analysis

Each function and constructor receives deterministic flow facts. The analysis
counts reachable and unreachable statements and determines whether control can
fall through the callable body. It diagnoses value-returning functions that do
not return on every reachable path. Statements after a guaranteed `return`,
`break`, or `continue` are retained for tooling, diagnosed as unreachable
warnings, and lowered into dead MIR blocks.

Functions with an omitted return annotation and functions explicitly returning
`void` share the same semantic type. They may fall through or terminate with a
valueless return. A void call lowers without a result value, so MIR cannot
accidentally feed it into a value-producing instruction. Constructor bodies use
the same valueless control-flow rules, while constructor call instructions still
produce the allocated object reference.

A `while` loop lowers to condition, body, and exit blocks. The condition
branches to the body or exit, body fallthrough and `continue` jump to the
condition, and `break` jumps to the exit. Loop targets form a stack, so nested
loops always resolve control statements to the innermost loop. A literal
`while (true)` cannot fall through unless its body contains a reachable
`break`.

An array `for` loop evaluates its iterable once, then lowers to condition, body,
latch, and exit blocks. A body entry performs a checked element load and stores
the value into the iteration local. Fallthrough and `continue` target the latch,
which increments an `int32` phi index before returning to the condition.
`break` targets the exit directly. This shape prevents `continue` from skipping
the hidden increment.

A classical `for` loop lowers its initializer in the preheader, followed by
condition, body, update, and exit blocks. An omitted condition jumps directly
to the body. Body fallthrough and `continue` target the update block; `break`
targets the exit. Updates execute from left to right before returning to the
condition. A conditionless loop cannot fall through unless it has a reachable
`break`.

## MIR structure

Every callable and field initializer owns a `MirBody`. IDs for basic blocks and
temporary values are local to that body. A body has one entry block and an
ordered block table. Every block contains ordered instructions followed by one
of these terminators:

- unconditional jump
- conditional branch
- return with an optional value
- unreachable

Instructions represent literals, symbol and member loads/stores, array
allocation/load/store/length operations, local declarations, unary and binary
operations, calls, explicit conversions, and phi values. Source ranges,
`TypeId`, and `SymbolId` identities survive lowering.

Stage 16.2 also carries each validated optional base `FileId` through HIR and
MIR so ABI lowering can schedule and flatten class layouts without
rediscovering semantic ancestry. Stage 16.3 gives constructor MIR an explicit
base-constructor call and a local-field initialization marker. The base call,
when present, must precede that marker; constructor-body statements follow it.
The MIR verifier checks the call against the semantic constructor binding and
requires exactly one field marker in every constructor.

Stage 15 adds explicit object widening, object metadata, type-test, and
checked-cast instructions. Stage 16.4 extends the same widening instruction to
direct, transitive, and nullable base conversions. The MIR verifier reconstructs
semantic ancestry and rejects unrelated source and target types. Every
reference widening erases to the same pointer. A checked cast retains both the
nullable result type and its non-null runtime target.

Stage 20.1 adds `kWidenNumeric` for typed primitive values. MIR inserts the
conversion at declarations, assignments, returns, calls, array elements,
iteration bindings, compound assignments, and mixed-width binary operations.
The verifier reconstructs the source and target numeric properties and rejects
anything except a lossless widening.

Stage 20.2 commits overload-directed numeric literals to the selected parameter
types before HIR lowering. Calls therefore receive exact-typed MIR literal
values; `kWidenNumeric` remains reserved for conversions of already typed
values.

Stage 20.3 retains `NumericType(value)` as a dedicated HIR expression. A
representable literal already has the target type and needs no runtime MIR
operation. An already typed operand lowers to `kCheckedNumeric`; the verifier
requires numeric source and target types. LLVM lowering performs the range
check before integer truncation, signedness changes, floating-to-integer
conversion, or finite `float64`-to-`float32` overflow.

Stage 30 retains `IntegerType::wrap(value)` and `IntegerType::sat(value)` as
dedicated HIR expressions and always lowers them to `kWrapInteger` or
`kSaturateInteger`, including identity conversions. The MIR verifier requires
integer source and target types, exact result metadata, and a value category.
The explicit mode therefore survives until target-independent LLVM lowering;
it is never replaced with `kCheckedNumeric`.

Evaluation is left-to-right. Array element operands are evaluated before the
array instruction, and indexed assignment evaluates the array and index before
the assigned value. Compound assignment and increment/decrement lower through
one captured location: member receivers and array/index operands are evaluated
once, followed by an explicit load, operation, and store. Postfix updates retain
the loaded value as their result; prefix updates return the stored value.
Structured loops use the same explicit jump and branch
terminators as other control flow. Instance-qualified calls carry an explicit
receiver; file-class-qualified calls do not. Nullable widening and proof-backed
narrowing are explicit MIR conversions; both erase at the ABI boundary. `&&`
and `||` lower to branches and a typed phi value, preserving short-circuit
behavior.

Nullable presence checks lower to explicit non-null tests. Safe member access
and null coalescing lower to guarded branches and typed phi values, so the
receiver or left operand is evaluated once and the skipped side is not
evaluated. Postfix non-null assertion lowers to a dedicated checked MIR
instruction and runtime guard before exposing the underlying reference type.

Calls retain whether they were unqualified, file-class-qualified,
instance-qualified, base-qualified, constructor allocation calls, or
base-constructor initialization calls. This avoids choosing an implicit
method calling convention before the ABI stage. Calls are marked as direct,
class-virtual, or interface dispatch. A class-virtual call retains the
statically selected declaration
and its stable slot; the backend chooses the runtime implementation through the
receiver descriptor. MIR also records whether that receiver is `self`, allowing
field initializers and constructors to suppress only dispatch on the object
currently being initialized.

An interface call retains the static interface `FileId` and flattened contract
slot in addition to its callable signature and explicit receiver. The MIR
verifier checks that the slot selects that exact contract and rejects interface
metadata on any non-interface call. Class-to-interface and child-to-parent
interface conversions use the existing pointer-preserving reference-widening
instruction.

Stage 16.6 base-qualified calls always carry direct dispatch, use implicit
`self`, and retain the declaration selected through the direct-base view. The
MIR verifier rejects a virtual base-qualified call or a symbol hidden by a
nearer declaration set.

Field initializers are lowered to independent MIR bodies that return the
initialized value. The constructor marker tells the LLVM backend where to
compose only the current file class's local initializer bodies. Recursive base
initializer entries handle inherited fields, so each field runs exactly once.
MIR does not assume the Stage 4 object layout.

MIR's explicit value types let the LLVM backend identify every reference-valued
parameter, binding, and temporary without scanning native stack bytes. Stage
13.2 uses that information to construct precise shadow-stack frames; root-frame
operations do not appear in target-independent MIR.

## Enum values

Enum locals require declaration initializers. The constructor field analysis
also tracks enum fields as required initialization, including reads, instance
calls, `self` escape, branches, and early returns. It never substitutes the
first case for a missing initializer.

HIR and MIR use typed enum literals carrying a canonical decimal tag. The
nominal type determines the case owner; verifiers reject out-of-range tags,
integer/enum literal confusion, and numeric operators on enum operands.
Equality requires matching nominal types and produces `bool`. Copies, call
arguments/results, and array operations retain that type through lowering.
Enum meta queries retain their evaluated operand even though the type name is
statically known. See [enums](enums.md).

## Deferred boundaries

Stage 3.0 does not define object size or alignment, calling
conventions, name mangling, exception handling, target machine types, LLVM IR,
machine code, runtime services, or garbage-collector barriers. Stage 4.0 defines
the data-layout and ABI boundary in
[data_layout_and_abi.md](data_layout_and_abi.md).
