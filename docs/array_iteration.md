# Cloth Stage 10.0 array iteration

Stage 10.0 adds declaration-based `for` iteration over arrays:

```cloth
for (var value in values) {
    print(value);
}

for (int32 value in values) {
    print(value);
}
```

## Binding contract

`var` infers the array's canonical element type. An explicit type is checked
with ordinary assignment compatibility. The iteration binding is a mutable
local copy created for each source-level iteration and is visible only in the
body. Reassigning it does not modify the array; indexed assignment remains the
explicit element-write operation.

The iterable expression is evaluated exactly once before the loop. It must have
array type. A null reference traps when the loop first queries `Length`, and an
empty array executes no iterations.

## Control flow

Lowering creates a preheader plus condition, body, latch, and exit blocks. An
`int32` phi value carries the hidden index. The condition compares it with
`Length`; the body performs a checked indexed load and initializes the loop
binding. Body fallthrough and `continue` target the latch, which increments the
index. `break` targets the exit.

This structure preserves left-to-right evaluation, executes the iterable only
once, and prevents `continue` from accidentally repeating the same element.

## Deferred work

Stage 10.0 iterates arrays only. Index/value dual bindings, numeric ranges,
destructuring, asynchronous iteration, and a general `Iterable<T>` protocol are
deferred. The `declaration in expression` syntax can support those features
without changing existing array source.
