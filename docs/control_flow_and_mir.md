# Cloth Stage 3.0 control flow and MIR

Stage 3.0 verifies typed HIR, performs callable-level control-flow analysis, and
lowers executable definitions to a target-independent mid-level intermediate
representation (MIR). MIR is the boundary between language semantics and future
target layout, ABI, optimization, and code generation.

## Verification

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

Evaluation is left-to-right. Array element operands are evaluated before the
array instruction, and indexed assignment evaluates the array and index before
the assigned value. Structured loops use the same explicit jump and branch
terminators as other control flow. Instance-qualified calls carry an explicit
receiver; file-class-qualified calls do not. Nullable widening and proof-backed
narrowing are explicit MIR conversions; both erase at the ABI boundary. `&&`
and `||` lower to branches and a typed phi value, preserving short-circuit
behavior.

Calls retain whether they were unqualified, file-class-qualified,
instance-qualified, or constructor calls. This avoids choosing an implicit
method calling convention before the ABI stage.

Field initializers are lowered to independent MIR bodies that return the
initialized value. The LLVM backend composes those bodies with constructor
execution using the Stage 4 object layout; MIR does not assume that layout.

## Deferred boundaries

Stage 3.0 does not define object size or alignment, vtables, calling
conventions, name mangling, exception handling, target machine types, LLVM IR,
machine code, runtime services, or garbage-collector barriers. Stage 4.0 defines
the data-layout and ABI boundary in
[data_layout_and_abi.md](data_layout_and_abi.md).
