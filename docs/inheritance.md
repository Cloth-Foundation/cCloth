# Cloth inheritance through Stage 17.2

Stage 16.1 introduces the declaration and semantic identity of single class
inheritance. Stage 16.2 carries that identity through HIR and MIR, defines the
physical base-object prefix, and links runtime type descriptors. Stage 16.3
defines explicit base-constructor chaining and complete-object initialization.
Stage 16.4 adds inherited member lookup, representation-preserving base
conversions, and hierarchy-aware checked type operations. Stage 16.5 adds an
explicit override contract, stable virtual slots, and dynamic instance calls.
Stage 16.6 adds explicit direct-base calls without changing object layout or
the virtual table. Stage 17.1 adds declaration identity for abstract file
classes and abstract instance functions. Stage 17.2 makes those declarations
enforceable by construction and subclass-completeness rules.

## File-class declaration

A source file may retain the existing unwrapped form or place all members in an
explicit class envelope:

```cloth
// User.co
class {
  string Name;
}
```

The file stem remains the class name. Repeating it as `class User { ... }` is an
error. The envelope is useful when the file class declares a base:

```cloth
import models::Human;

class : Human {
  string Name;
}
```

Imports remain file-scoped and must precede the class declaration. The class
body must consume the remainder of the file, and a file may contain at most one
explicit envelope. Omitting the envelope remains exactly equivalent to an
explicit root `class { ... }`.

## Semantic graph

The optional name after `:` uses ordinary same-package and import resolution,
including explicit aliases and wildcard imports. It must resolve to a visible
file class. Lowercase file classes remain private to their defining file and
therefore cannot be used as another file's base.

Each `FileSemantics` records at most one `base_file`, producing a directed
single-inheritance graph over stable `FileId` values. Semantic analysis rejects
an unknown or inaccessible base, direct self-inheritance, and every indirect
cycle. Cycle traversal begins in qualified-name order so diagnostics do not
depend on source discovery order.

## Layout and descriptor contract

Every derived object starts with the complete layout of its direct base,
including the two-word runtime header and any padding at the end of the base.
Derived instance fields begin at or after the base size and follow the ordinary
declaration-order alignment rules. Base tail padding is not reused. This makes
the base subobject a stable byte-for-byte prefix and keeps a future upcast
representation preserving.

The derived ABI field table is flattened: inherited fields appear first with
their original symbols, types, and offsets, followed by local fields. Static
fields never enter either layout. The derived descriptor likewise contains the
complete ordered reference-offset list for inherited and local fields, so the
collector needs no hierarchy walk while tracing an object.

Each file-class descriptor also contains a nullable parent pointer. A root
class stores null; a derived class points at the compiler-emitted descriptor of
its direct base. Descriptor identity stays canonical within one emitted module.
The ABI verifier reconstructs the hierarchy layout, checks the exact base
prefix, and checks that descriptor ancestry agrees with the semantic graph.
Layout scheduling is independent of source discovery order.

## Constructor contract

Every declared constructor of a derived file class explicitly selects one
constructor of its direct base:

```cloth
class : Human {
  User(string name, int32 age): Human(name, age) {
    // User constructor body.
  }
}
```

The name after `:` must resolve to the direct base class. Arguments use normal
constructor overload selection and are evaluated left to right. A root-class
constructor cannot have a base initializer. Cloth does not currently synthesize
constructors, so a derived file class with no declared constructor remains
legal but cannot be constructed. When a derived constructor is declared, its
base initializer is mandatory; Cloth does not silently select a zero-argument
base constructor.

Base-initializer expressions run before the object has completed base
initialization. They may use constructor parameters, literals, and static
functions, but may not observe `self`, instance fields, or unqualified instance
functions.

One call to the public, most-derived constructor allocates exactly one object
using the most-derived descriptor. Initialization then occurs in this order:

1. Evaluate the direct-base initializer arguments from left to right.
2. Invoke the selected base initializer on the same object.
3. The base initializer repeats steps 1 and 2 for its own base. At the root, it
   runs local field initializers in declaration order and then its constructor
   body.
4. As the chain returns, each class runs its local field initializers and
   constructor body in base-to-derived order.

Thus initializer arguments are encountered from derived toward the root, while
fields and bodies execute from the root toward the most-derived class. An early
`return` ends only the current constructor body; successful base initialization
still returns control to the derived initialization sequence. A runtime trap
aborts the sequence normally.

## Lowering contract

HIR records the selected base-constructor symbol and typed arguments. MIR makes
the ordering explicit with a base-constructor call followed by a local-field
initialization marker before the constructor body. Its verifier requires
exactly one marker per constructor, the semantic base call when present, and
the base-before-fields ordering.

The ABI gives each constructor two entries. The public `_C1C` entry accepts
declared parameters, allocates the complete object, initializes it, and returns
the reference. A private `_C1I` initializer entry accepts `self` followed by
the declared parameters and returns `void`. Base chaining calls `_C1I` on the
same object, preventing a second allocation and preserving the most-derived
descriptor throughout construction. Both entries root `self` for the complete
initialization sequence.

## Inherited member lookup

Lookup begins at the receiver's static file-class type and walks toward the
root. The first class declaring the requested name supplies the complete field
or function overload set; that declaration set hides classes farther up the
chain. Constructors are never inherited.

Capitalization visibility applies at the declaration owner. Public inherited
members are available to derived classes and external callers. A private member
is available only while analyzing its declaring file class, including through
a receiver whose static type is derived from that class. It is not promoted to
protected visibility merely because another class derives from its owner.

Inherited instance fields use their existing flattened base offsets. Inherited
instance calls pass the unchanged receiver pointer to the selected base ABI
entry. Static members may be named through a derived file-class type but retain
their declaring class's storage or callable identity.

The nearest declaration set still controls overload resolution. After one
function is selected, however, a public instance function call dispatches
through its stable virtual slot. The static receiver type therefore determines
which signature is callable, while the runtime descriptor determines which
compatible implementation runs.

## Override contract

Every public instance function is virtual. Private functions and static
functions are never virtual. A public function that has the same name and
canonical parameter types as an inherited virtual function must be written
with `override`:

```cloth
class : Human {
  override func Describe(): string {
    return "User";
  }
}
```

The return type must also match exactly. Cloth does not currently support
covariant returns. Omitting `override` on a matching signature is an error,
and using it when no inherited target exists is an error. A different parameter
signature is an overload and receives a new virtual slot. Capitalization still
governs visibility, so private lowercase functions cannot override or be
overridden.

Each root class assigns slots to its public instance functions in declaration
order. A derived class begins with its base table, replaces a matched override
in place, and appends new public instance functions in declaration order. The
compiler records the immediate overridden symbol and the stable slot on every
override. Type descriptors carry the resulting implementation table and slot
count.

An instance call loads the object's most-derived descriptor, loads its virtual
table, and invokes the selected slot with the unchanged receiver pointer. Thus
both `derived.Describe()` and an upcast `base.Describe()` reach the same
most-derived override. Static and private functions remain direct ABI calls.

Field initializers and constructor bodies deliberately suppress virtual
dispatch only for calls on the object currently being initialized. Those calls
bind directly to the declaration selected for that class, preventing an
override from observing derived fields before initialization completes. Calls
on unrelated receivers remain virtual, as do all instance calls in ordinary
function bodies.

## Base-qualified calls

An instance context may use `super` to invoke one base implementation
explicitly:

```cloth
class : Human {
  override func Describe(): string {
    return super.Describe() + " -> User";
  }
}
```

`super` is a reserved expression representing the current file class's
direct-base view. It cannot be rebound, and a named class cannot substitute for
it. This prevents a transitive ancestor from being named to skip an
intermediate base. Lookup retains the ordinary nearest-declaration-set and
overload rules. Consequently, `super.Method()` may select a public declaration
in an ancestor when the direct base does not declare that name itself.

The selected function must be a public instance function. The form is invalid
in a static function and in a base-constructor initializer, where no usable
`self` exists. `Human.StaticFunction()` remains an ordinary class-qualified
static call. `super.StaticFunction()`, `super.Field`, and `super(...)` are
invalid; static functions, fields, and constructors do not gain a
corresponding direct-base access form.

A base-qualified call passes the current `self` pointer unchanged and invokes
the selected ABI symbol directly. It bypasses the virtual table for that call
only; calls made from inside the selected base implementation follow their
ordinary dispatch rules. HIR retains the qualification, MIR uses a dedicated
base-qualified call kind with direct dispatch, and the MIR verifier checks the
direct-base lookup and implicit-self invariants.

## Abstract declarations

An abstract file class must use an explicit envelope:

```cloth
abstract class {
  abstract func Describe(): string;
}
```

An abstract function has no source body and ends with `;`. It must be a public
instance function declared by an abstract file class. Capitalization therefore
remains meaningful: a lowercase abstract function is private and invalid.
Static abstract functions are also invalid. Concrete functions in the same
abstract class continue to use ordinary blocks.

Abstract functions participate in normal signature registration and receive
stable virtual slots. AST, semantic symbols, HIR, MIR, and diagnostic summaries
retain their abstract identity. MIR represents the absent implementation with
a verified unreachable stub so no empty concrete implementation is invented.
A direct `super.AbstractFunction()` call is invalid because there is no base
implementation to invoke.

An abstract file class cannot be constructed directly. Its constructors remain
ordinary declarations because a derived constructor must still initialize the
abstract base subobject explicitly:

```cloth
abstract class {
  Shape(int32 scale) {}
  abstract func Area(): float;
}
```

```cloth
class : Shape {
  Circle(): Shape(1) {}

  override func Area(): float {
    return 1.0;
  }
}
```

After override resolution, each file class records the virtual slots whose
selected declarations remain abstract. An abstract subclass may implement any
subset and carry the rest forward. A concrete subclass must leave that set
empty. Missing implementations are reported by canonical signature with a note
at the nearest abstract declaration, so overloads remain unambiguous and source
discovery order cannot affect diagnostics.

## Subtyping and checked operations

A non-null derived reference implicitly widens to any direct or transitive base
type. The conversion also composes with nullability: `Derived` converts to
`Base?`, and `Derived?` converts to `Base?`. Conversions in the opposite
direction remain explicit and checked. Unrelated file classes remain
incompatible, and arrays remain invariant.

The base-prefix layout makes every widening representation preserving: MIR
records a checked reference-widening instruction, while LLVM reuses the same
pointer without adjustment. The object's descriptor remains the most-derived
descriptor, so allocation identity, garbage-collector metadata, and
`::typeName` do not change after an upcast.

File-class `is` and nullable `as` operations now follow descriptor parent links.
Consequently, an object satisfies its exact class and every transitive base,
while a base-typed reference can still be tested or safely cast back to its
actual derived type. Null and unrelated runtime kinds fail without trapping.

## Deliberate Stage 17.2 boundary

`sealed` is a reserved file-class modifier but remains a diagnosed unsupported
contract until Stage 17.3, which also owns final overrides. Covariant returns
and interface dispatch remain separate language work.
