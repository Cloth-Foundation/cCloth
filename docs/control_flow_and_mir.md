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
not return on every reachable path. Statements after a guaranteed return are
retained for tooling, diagnosed as unreachable warnings, and lowered into dead
MIR blocks.

## MIR structure

Every callable and field initializer owns a `MirBody`. IDs for basic blocks and
temporary values are local to that body. A body has one entry block and an
ordered block table. Every block contains ordered instructions followed by one
of these terminators:

- unconditional jump
- conditional branch
- return with an optional value
- unreachable

Instructions represent literals, symbol and member loads/stores, local
declarations, unary and binary operations, calls, explicit conversions, and phi
values. Source ranges, `TypeId`, and `SymbolId` identities survive lowering.

Evaluation is left-to-right. Instance-qualified calls carry an explicit
receiver; file-class-qualified calls do not. Nullable-to-reference coercion is
an explicit MIR conversion. `&&` and `||` lower to branches and a typed phi
value, preserving short-circuit behavior.

Calls retain whether they were unqualified, file-class-qualified,
instance-qualified, or constructor calls. This avoids choosing an implicit
method calling convention before the ABI stage.

Field initializers are lowered to independent MIR bodies that return the
initialized value. A later object-layout stage will decide how those bodies are
composed with constructor execution; MIR does not assume an object layout.

## Deferred boundaries

Stage 3.0 does not define object size or alignment, vtables, calling
conventions, name mangling, exception handling, target machine types, LLVM IR,
machine code, runtime services, or garbage-collector barriers. The next stage
should define a portable data-layout and ABI contract before selecting a
backend.
