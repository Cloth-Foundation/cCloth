## Cloth Language Design — Visibility & Declarations

This document defines the current design for **declarations, visibility, and function syntax** in the Cloth programming language.

---

## 1. Design Goals

Cloth aims for:

* **Readable, sentence-like declarations**
* **Minimal syntactic noise**
* **Clear separation of concerns**:

  * Visibility (who can see it)
  * Binding (can it be reassigned?)
  * Type (what it is)
  * Qualifiers (how it behaves in memory)
* **No “annotation soup” or type-heavy syntax**
* **Predictable compilation model**

---

## 2. Visibility Model

Cloth uses **visibility blocks** to group declarations by access level without repeating keywords.

Visibility blocks **do not create scope**.
They apply a visibility attribute to the declarations inside them.

### Example

```cloth
public {
    var x: atomic long? = 10;
}

private {
    func foo(): void {
        println("Hello, world! " + x);
    }
}

internal {
    func bar(): i32 {
        foo();
        return 0;
    }
}
```

### Meaning

| Symbol | Visibility                                 |
| ------ | ------------------------------------------ |
| `x`    | exported outside the module                |
| `foo`  | visible only within this module            |
| `bar`  | visible within the package/module boundary |

`bar()` **can call** `foo()` because visibility blocks are not scopes.

---

## 3. Visibility Keywords

Cloth supports three visibility levels:

| Keyword    | Meaning                                  |
| ---------- | ---------------------------------------- |
| `public`   | Exported outside the module              |
| `internal` | Visible within the module/package        |
| `private`  | Visible only within the declaring module |

### Default Visibility

If no visibility is specified, the declaration is **`internal`**.

```cloth
func helper() {}   // implicitly internal
```

---

## 4. Visibility Block Rules

### Allowed

```cloth
public {
    func api() {}
    struct Data {}
}
```

### Not Allowed (no nesting)

```cloth
public {
    private {
        func badIdea() {}
    }
}
```

If a declaration needs a different visibility, declare it outside the block.

### Not Allowed (no local visibility blocks)

```cloth
func f() {
    public { var x: i32 = 0; }  // invalid
}
```

Visibility only applies to **top-level or type-level declarations**.

---

## 5. Declaration Syntax

All declarations follow the same readable structure:

```
[binding] name : [qualifiers] type [?] = initializer
```

### Examples

```cloth
var counter: i64 = 0;
let name: string = "Cloth";
const maxSize: u32 = 1024;
```

---

## 6. Binding Keywords

Binding controls reassignment semantics.

| Keyword | Meaning               |
| ------- | --------------------- |
| `var`   | Mutable binding       |
| `let`   | Immutable binding     |
| `const` | Compile-time constant |

Example:

```cloth
var x: i32 = 5;   // may change
let y: i32 = 5;   // cannot change
const z: i32 = 5; // constant expression
```

---

## 7. Type Annotation

Types follow the identifier using `:`.

```cloth
var value: i32 = 10;
```

This keeps the identifier visually prominent and avoids C-style declaration confusion.

---

## 8. Type Qualifiers

Qualifiers modify how the value behaves in memory.

They appear **before the base type**.

```cloth
var counter: atomic i64 = 0;
```

### Current Qualifiers

| Qualifier | Meaning                         |
| --------- | ------------------------------- |
| `atomic`  | Enables atomic memory semantics |

Additional qualifiers may be introduced later without altering syntax shape.

---

## 9. Nullability

Nullability is a **type property**, indicated by `?`.

```cloth
var name: string? = null;
var counter: atomic i64? = 0;
```

This means the variable may contain either:

* A value of the base type, or
* `null`

### Why `?` is attached to the type

Nullability is part of type-checking, not value syntax.

Correct:

```cloth
var x: i32? = 10;
```

Not allowed:

```cloth
var x: i32 = 10?;   // invalid
```

---

## 10. Function Declarations

Functions follow the same naming-first structure as variables.

### Syntax

```cloth
func name(parameters): returnType {
    ...
}
```

### Example

```cloth
func add(a: i32, b: i32): i32 {
    return a + b;
}
```

### Void Return

```cloth
func greet(): void {
    println("Hello!");
}
```

(An optional future simplification may allow omitting `: void`.)

---

## 11. Parameters

Parameters follow the same pattern as variables:

```cloth
name: type
```

Example:

```cloth
func scale(value: f32, factor: f32): f32 {
    return value * factor;
}
```

---

## 12. Design Philosophy Summary

Cloth separates concerns cleanly:

| Concept     | Expressed By               |
| ----------- | -------------------------- |
| Visibility  | Visibility blocks          |
| Mutability  | `var / let / const`        |
| Type        | `name: type`               |
| Behavior    | Type qualifiers (`atomic`) |
| Nullability | `?` suffix                 |
| Execution   | `func` declarations        |

Each feature answers **one question only**, preventing keyword overload and keeping the language readable at scale.

---

## 13. Future Extensions (Non-breaking)

This model allows adding:

* More qualifiers (`shared`, `volatile`, etc.)
* Generic types
* Module/import systems
* Visibility refinements

Without altering declaration shape.
