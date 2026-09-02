# Cloth interfaces

Stage 18 adds interfaces as first-class file types. An interface defines a
public behavioral contract without storage, construction, or a default method
implementation. Classes retain single implementation inheritance and may
conform to any number of interfaces.

## File identity and syntax

An interface uses the source file's existing type identity:

```cloth
// Renderable.co defines Renderable.
interface {
  func Render(int32 width): string;
}
```

The file name is not repeated. Interface functions are public instance
contracts and end with `;`. Capitalization therefore remains meaningful: a
lowercase function is private and invalid in an interface. Fields,
constructors, static functions, function modifiers, and function bodies are
not interface members.

An interface may inherit multiple interfaces:

```cloth
interface : Named, Resettable {
  func Render(int32 width): string;
}
```

A class keeps its optional single base after `:` and lists directly declared
interfaces after `is`:

```cloth
class : WidgetBase is Renderable, Serializable {
  // Members.
}
```

The explicit class envelope is required for a conformance clause. A class
inherits all conformance declared by its base class. An interface inherits all
contracts and subtype relationships declared by its parent interfaces.

## Contract identity and composition

An interface function is identified by its name and canonical parameter types.
Return types do not create overloads. Overloads with different parameter lists
remain distinct contracts.

When multiple parent interfaces contribute the same signature, Cloth
coalesces the requirement. Compatible covariant managed-reference returns
select the narrowest contract. Primitive and `void` returns must match exactly;
unrelated returns are a declaration error. A child interface may redeclare a
signature to refine its return under the same covariance rules.

Interface inheritance cycles, duplicate direct parents, and class names in an
interface list are errors. Resolution uses ordinary imports, package
visibility, and deterministic file identity.

## Class conformance

A public instance function satisfies an interface requirement when its name
and canonical parameter types match and its return type is assignable to the
contract return. A locally declared implementation must use `override`, whether
it satisfies an interface contract, replaces an inherited class function, or
does both:

```cloth
// Widget.co
class is Renderable {
  override func Render(int32 width): string {
    return "Widget";
  }
}
```

The marker is checked: a function with no matching class or interface contract
cannot use `override`. One declaration may satisfy several interfaces with the
same signature. Overloads are checked separately. Return types remain subject
to the existing covariance rules; the marker does not allow numeric widening.

An interface-only implementation introduces an ordinary class virtual slot; it
does not acquire a base-class implementation. `super` still selects a class
ancestor and cannot call an interface contract. `final override func` may seal
an interface implementation against further replacement.

An abstract class that restates a requirement uses `abstract override func`.
An interface that refines an inherited requirement still uses plain `func`:
interface members do not accept modifiers. Inherited class implementations need
no redundant redeclaration, including when the base class did not itself name
the interface. This requirement applies equally to source-free dependencies.

Migration: add `override` to existing locally declared interface implementations.
There is no new `impl` keyword.

A concrete class must satisfy every requirement from every direct, inherited,
and transitive interface. An abstract class may carry unresolved interface
requirements; the first concrete descendant must complete them. A derived
override replaces the implementation used by every inherited interface table,
so calls through class and interface references observe the same most-derived
behavior.

Interfaces do not provide default bodies. Adding executable interface members
would require a separate conflict-resolution, diamond-inheritance, and
base-qualification contract and is not implied by Stage 18.

## Reference conversions and checked operations

Interface values are ordinary managed object references. They do not use a fat
pointer and do not change object identity.

- A class reference widens to any interface the class conforms to.
- An interface reference widens to any transitive parent interface.
- Every interface reference widens to `object`.
- Nullability composes with these conversions using the existing `T?` rules.
- Conversion from an interface to a class or sibling interface requires
  `as T?`; a non-null test uses `is T`.

Runtime checks inspect the most-derived class descriptor. Unrelated interface
types may overlap because one class may implement both. A sealed class and an
unimplemented interface cannot overlap.

## Dispatch and runtime representation

Each interface receives a deterministic 64-bit identity derived from its
[canonical nominal identity](canonical_identity.md), including exact manifest
package version or standalone ownership. The compiler diagnoses an identity
collision within a compilation. Each interface also owns a deterministic
flattened contract-slot order: inherited contracts precede locally introduced
contracts, and duplicate signatures are coalesced.

Every concrete class descriptor contains one dispatch entry for each
transitive interface. Entries are sorted by interface identity for binary
lookup. An entry contains the interface identity, a function-pointer table in
that interface's contract-slot order, and the slot count. Derived descriptors
flatten inherited interface entries and replace function pointers with the
most-derived class implementations.

An interface call carries its static interface identity and contract slot
through HIR and MIR. LLVM lowering asks the runtime for that function pointer,
then invokes it with the unchanged object receiver. Runtime lookup traps if
compiler-generated metadata is inconsistent; a well-typed program cannot miss
the requested entry or slot.

This representation leaves the object header, field offsets, class virtual
table, garbage-collector metadata, and reference representation unchanged.
