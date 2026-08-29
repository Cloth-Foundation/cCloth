# Cloth Stage 11 void and callable contracts

Stage 11 exposes `void` as the canonical return type for functions that produce
no value. It also prevents no-result calls from being used as values.

## Function returns

These declarations have identical semantic and ABI types:

```cloth
func Implicit() {}
func Explicit(): void {}
```

Omitting a function return annotation defaults to `void`; Cloth never infers a
return type from `return` statements. A void function may fall through or use
`return;`. It may not use `return expression;`.

Constructors use `Init(...)` or `init(...)` without a return annotation. Their
bodies follow void return rules, while a type-name constructor call produces
the new file-class object.

## Non-value rule

`void` describes absence of a value and has no source storage. It is invalid as
a field, parameter, local, array element, or iteration binding type. A call
whose return type is void is valid as an expression statement but invalid in
an initializer, argument, array literal, operator, condition, or value return.

```cloth
func Write(): void { println("cloth"); }

func Valid(): void {
    Write();
}

void InvalidField;             // invalid
func Invalid(void value) {}    // invalid
int32 result = Write();        // invalid
```

Cloth uses `()` for an empty parameter list; it does not adopt C's special
`func Name(void)` spelling. `void` is also distinct from future unit, never,
and type-erasure types.

## Compiler representation

Explicit and omitted void returns share one `TypeId` and `TypeKind::kVoid`.
HIR permits the void type only on calls and grouped void calls. MIR emits void
calls without result IDs and uses valueless return terminators. The ABI assigns
void size zero, alignment one, and LLVM lowers it to `void`, `call void`, and
`ret void` on both x86-64 and wasm32.

Native entry points accept `static func Main(): void`, omitted-return
`static func Main()`, or `static func Main(): int32`. The first two return
process status zero; the last supplies the process status explicitly.
