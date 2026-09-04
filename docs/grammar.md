# Implemented Cloth grammar

This document defines the currently implemented syntax.
Contextual rules are listed separately and are not encoded into EBNF.

## Lexical forms

```ebnf
identifier
    = identifier_start { identifier_continue } ;

identifier_start
    = ascii_letter
    | "_" ;

identifier_continue
    = ascii_letter
    | decimal_digit
    | "_" ;

ascii_letter
    = "A" ... "Z"
    | "a" ... "z" ;

decimal_digit
    = "0" ... "9" ;
```

Literal token formation, whitespace, comments, and escape sequences are defined
by the lexer. Keywords cannot be used as identifiers.

## Declarations

```ebnf
compilation_unit
    = { import_declaration }
      ( explicit_file_type | { member_declaration } ) ;

explicit_file_type
    = explicit_file_class
    | explicit_interface
    | explicit_enum
    | explicit_struct ;

explicit_struct
    = "struct" "{" { member_declaration } "}" ;

explicit_enum
    = "enum" "{" enum_case { "," enum_case } [ "," ] "}" ;

enum_case
    = identifier ;

explicit_file_class
    = { file_class_modifier } "class" [ ":" named_type ]
      [ "is" named_type { "," named_type } ]
      "{" { member_declaration } "}" ;

explicit_interface
    = "interface" [ ":" named_type { "," named_type } ]
      "{" { interface_function_declaration } "}" ;

interface_function_declaration
    = "func" identifier "(" [ parameter_list ] ")"
      [ ":" return_type ] ";" ;

file_class_modifier
    = "abstract"
    | "sealed" ;

import_declaration
    = "import" identifier [ "as" identifier ] ";"
    | "import" package_path "::" identifier
      [ "as" identifier ] ";"
    | "import" package_path "." "*" ";" ;

package_path
    = identifier { "." identifier } ;

member_declaration
    = field_declaration
    | function_declaration
    | constructor_declaration ;

field_declaration
    = [ "static" ] [ "final" ] type identifier
      [ "=" expression ] ";" ;

function_declaration
    = { function_modifier } "func" identifier
      "(" [ parameter_list ] ")"
      [ ":" return_type ]
      ( block | ";" ) ;

function_modifier
    = "static"
    | "override"
    | "abstract"
    | "final" ;

return_type
    = type
    | "void" ;

constructor_declaration
    = identifier
      "(" [ parameter_list ] ")"
      [ ":" type "(" [ argument_list ] ")" ]
      block ;

parameter_list
    = parameter { "," parameter } ;

parameter
    = [ "final" ] type identifier ;

type
    = element_type [ "?" ] [ "[" "]" [ "?" ] ] ;

element_type
    = primitive_type
    | named_type ;

primitive_type
    = "bool" | "byte" | "char"
    | "int" | "int8" | "int16" | "int32" | "int64"
    | "uint" | "uint8" | "uint16" | "uint32" | "uint64"
    | "float" | "float32" | "float64" ;

named_type
    = identifier ;
```

At file scope, `class` opens the optional envelope for the implicit file class,
while `interface` and `enum` select their respective file kinds. The file name is
never repeated. Inside an envelope or after an unwrapped member, `class` and
`interface` are nested-type declaration starters. Nested enums and `struct`
declarations are also diagnosed as unsupported. An enum envelope accepts only
its ordered cases; it has no modifiers, base, conformance, fields, or functions.

`void` is accepted only as a function return type. An omitted function return
type defaults to `void`. Fields, parameters, locals, arrays, and iteration
bindings require a value-producing `type`.

Imports must precede the explicit class declaration or every unwrapped member
declaration. An explicit class body consumes the remainder of the file. A
`module` declaration is not part of Cloth: the source path relative to the
project source root supplies the package identity.

Function modifiers may appear in any order but may not be repeated.
`override` is required on a locally declared public class instance function that
matches a base-class or interface name and canonical parameter signature. It is
invalid without such a contract or when combined with `static`. Abstract class
restatements also require it; interface declarations themselves remain plain
`func`. An inherited implementation needs no redeclaration. Semantic analysis
enforces the complete [override contract](interfaces.md#class-conformance).
An override may keep the inherited return type or use a covariant managed-
reference return type that is assignable to it. Primitive and `void` returns
must match exactly.

On a function, `final` must be paired with `override`. The declaration seals its
virtual slot against replacement, including a newly introduced interface-only
implementation slot.
Function `final` is contextual and does not change the existing field,
parameter, local, or iteration-binding forms.

An abstract file class uses the explicit `abstract class { ... }` envelope. An
`abstract func` is a public instance declaration in an abstract file class and
ends with `;` instead of a body. Concrete functions still require a block.
A `sealed class` is concrete and cannot be named as another file class's base.
`abstract sealed class` and `abstract final override func` are contradictory
and rejected.

An interface function is implicitly abstract and accepts no function
modifiers. It must be public, non-static, and bodyless. Interfaces cannot
declare fields or constructors. An interface may inherit multiple visible
interfaces after `:`, while a class declares conformance after `is`. A class
may retain one implementation base and any number of interfaces.

## Statements

```ebnf
block
    = "{" { statement } "}" ;

statement
    = local_variable_statement
    | return_statement
    | if_statement
    | while_statement
    | for_statement
    | switch_statement
    | break_statement
    | continue_statement
    | expression_statement
    | block ;

local_variable_statement
    = local_variable_declaration ";" ;

local_variable_declaration
    = [ "final" ] ( type | "var" ) identifier
      [ "=" expression ] ;

return_statement
    = "return" [ expression ] ";" ;

if_statement
    = "if" "(" expression ")" block [ "else" block ] ;

while_statement
    = "while" "(" expression ")" block ;

for_statement
    = for_each_statement
    | classical_for_statement ;

for_each_statement
    = "for" "(" iteration_declaration "in" expression ")" block ;

classical_for_statement
    = "for" "(" [ for_initializer ] ";"
      [ expression ] ";" [ update_list ] ")" block ;

for_initializer
    = local_variable_declaration
    | expression ;

update_list
    = expression { "," expression } ;

iteration_declaration
    = [ "final" ] ( "var" identifier | type identifier ) ;

break_statement
    = "break" ";" ;

continue_statement
    = "continue" ";" ;

expression_statement
    = expression ";" ;

switch_statement
    = "switch" "(" expression ")" "{" switch_arm { switch_arm } "}" ;

switch_arm
    = "case" expression { "," expression } ":" block
    | "default" ":" block ;
```

Braces and semicolons shown above are mandatory. `break` targets the nearest loop
or switch; `continue` targets the nearest loop, skipping switches. Nested type
declarations inside blocks remain deferred.

Switch selectors are integers or enums; labels are
restricted constants despite their expression grammar. Default is unique and
last, enum coverage is exhaustive, and arm bodies do not fall through. Limits,
normalization, and recovery follow the [approved switch contract](proposals/stage_27_switch.md).

## Expressions

```ebnf
expression
    = assignment_expression ;

assignment_expression
    = null_coalescing_expression
      [ assignment_operator assignment_expression ] ;

assignment_operator
    = "=" | "+=" | "-=" | "*=" | "/=" | "%="
    | "<<=" | ">>=" | "&=" | "|=" | "^=" ;

null_coalescing_expression
    = logical_or_expression [ "??" null_coalescing_expression ] ;

logical_or_expression
    = logical_and_expression { "||" logical_and_expression } ;

logical_and_expression
    = bitwise_or_expression { "&&" bitwise_or_expression } ;

equality_expression
    = comparison_expression
      { ( "==" | "!=" ) comparison_expression } ;

comparison_expression
    = shift_expression
      { ( "<" | "<=" | ">" | ">=" ) shift_expression
      | "is" type
      | "as" type } ;

additive_expression
    = multiplicative_expression
      { ( "+" | "-" ) multiplicative_expression } ;

multiplicative_expression
    = unary_expression
      { ( "*" | "/" | "%" ) unary_expression } ;

shift_expression
    = additive_expression { ( "<<" | ">>" ) additive_expression } ;

bitwise_and_expression
    = equality_expression { "&" equality_expression } ;

bitwise_xor_expression
    = bitwise_and_expression { "^" bitwise_and_expression } ;

bitwise_or_expression
    = bitwise_xor_expression { "|" bitwise_xor_expression } ;

unary_expression
    = ( "!" | "+" | "-" | "~" | "++" | "--" ) unary_expression
    | postfix_expression ;

postfix_expression
    = primary_expression
      { call_suffix | member_suffix | meta_suffix | safe_member_suffix | index_suffix
      | "!" }
      [ "++" | "--" ] ;

call_suffix
    = "(" [ argument_list ] ")" ;

argument_list
    = expression { "," expression } ;

member_suffix
    = "." identifier ;

meta_suffix
    = "::" identifier ;

safe_member_suffix
    = "?." identifier ;

index_suffix
    = "[" expression "]" ;

array_literal
    = "[" [ expression { "," expression } ] "]" ;

primary_expression
    = identifier
    | "super"
    | integer_conversion
    | numeric_conversion
    | integer_literal
    | float_literal
    | string_literal
    | character_literal
    | "true"
    | "false"
    | "null"
    | array_literal
    | "(" expression ")" ;

numeric_conversion
    = numeric_type "(" expression ")" ;

integer_conversion
    = integer_type "::" ( "wrap" | "sat" )
      "(" expression ")" ;

integer_type
    = "byte"
    | "int" | "int8" | "int16" | "int32" | "int64"
    | "uint" | "uint8" | "uint16" | "uint32" | "uint64" ;

numeric_type
    = "byte"
    | "int" | "int8" | "int16" | "int32" | "int64"
    | "uint" | "uint8" | "uint16" | "uint32" | "uint64"
    | "float" | "float32" | "float64" ;
```

The precedence table, from lowest to highest, is:

| Precedence | Operators                              | Associativity |
|-----------:|----------------------------------------|---------------|
| 1          | assignment operators                   | right         |
| 2          | `??`                                   | right         |
| 3          | `||`                                   | left          |
| 4          | `&&`                                   | left          |
| 5          | `|`                                    | left          |
| 6          | `^`                                    | left          |
| 7          | `&`                                    | left          |
| 8          | `==`, `!=`                             | left          |
| 9          | `<`, `<=`, `>`, `>=`, `is T`, `as T`  | left          |
| 10         | `<<`, `>>`                             | left          |
| 11         | `+`, `-`                               | left          |
| 12         | `*`, `/`, `%`                          | left          |
| 13         | prefix `!`, `+`, `-`, `~`, `++`, `--` | right         |
| 14         | calls, members, meta queries, indexing, postfix `!`, `++`, `--` | left |

Arithmetic, bitwise, and shift compound assignments are implemented. Numeric
increment and decrement are also implemented. Integer operator and endian meta
operation constraints are defined in `integer_binary_data.md`.

## Contextual constraints

The declaration pass enforces these rules separately from the grammar:

- The source file stem must be a valid Cloth identifier.
- An explicit `class` declaration never repeats the implicit file-class name.
- An explicit `interface` declaration never repeats the implicit file-type
  name. It may contain only public instance function contracts.
- `abstract func` is permitted only in an `abstract class`, must be public and
  non-static, and cannot have a body.
- An abstract file class cannot be constructed directly. A concrete subclass
  must override every inherited abstract virtual signature; abstract
  subclasses may carry unresolved signatures forward.
- A sealed file class cannot be inherited. An abstract file class cannot also
  be sealed.
- `final` on a function requires `override`; a final override remains callable
  but cannot itself be overridden. An abstract function cannot be final.
- An override return may narrow an inherited managed-reference return while
  preserving assignment compatibility. It cannot widen nullability or class
  identity; array types remain invariant. Primitive and `void` returns are
  exact.
- Integer and floating literals default to `int32` and `float64`, but adopt a
  representable expected numeric type in typed value contexts. Primitive values
  widen only when every source value is exactly representable by the target.
  Narrowing and cross-family conversion remain invalid implicitly.
  `NumericType(value)` explicitly requests a checked numeric conversion.
- A class base clause names at most one visible file class. A class conformance
  clause and an interface inheritance clause name visible interfaces. Neither
  the class graph nor the interface graph may contain a cycle.
- A concrete class satisfies every transitive interface function by public
  name, canonical parameter types, and a compatible covariant return. Abstract
  classes may carry unresolved interface requirements to a concrete subclass.
- Interface references widen from conforming classes and child interfaces,
  compose with nullability, and use checked `is`/`as` in the reverse direction.
- Member lookup uses the nearest class in the base chain that declares a name.
  Private declarations remain visible only in their owning file class.
- `super.Method(arguments)` is a direct-base call when `Method` selects a
  public instance function through the current file class's direct-base view.
  It requires an implicit receiver, cannot appear in a base-constructor
  initializer, and bypasses virtual dispatch for that call. Named
  type-qualified instance calls are invalid; type-qualified static calls retain
  their ordinary meaning.
- Derived references implicitly widen to direct or transitive base types,
  including compatible nullable forms. The reverse conversion requires `as`.
- A constructor name is derived from its implicit file-class name. In
  `User.co`, `User` declares a public constructor, while `user` and `_User`
  declare private constructors. Other constructor names are invalid.
  Constructor calls always use the implicit file-class name, such as
  `User(name)`.
- Every declared constructor in a derived file class must include a base
  initializer naming its direct base. Root constructors cannot include one.
- Base-initializer arguments use ordinary constructor overload selection and
  cannot access `self`, instance fields, or unqualified instance functions.
- Declaration visibility is inferred from the first ASCII character, except
  enum cases, which are always public. The enum type retains filename visibility.
- Enums contain 1 to 65,536 distinct, case-sensitive identifiers. Case selection
  uses `Type.Case`. Enum locals require initializers; enum fields require
  initialization on every constructor exit. See [enums](enums.md).
- Struct envelopes use filename identity and ordinary member visibility. All
  instance fields and struct locals require initialization. Struct methods have
  read-only value receivers; constructors initialize writable fields. Inheritance,
  conformance, abstract/override/final functions, and base initializers are invalid.
  See [structs](structs.md) for initialization, copy, and receiver rules.
- Private constructors are callable only inside their owning file class. A
  derived constructor cannot select a private base constructor.
- Constructor overload identity ignores the public or private constructor
  spelling; declarations cannot differ only by visibility.
- Conflicting fields and exact duplicate callable signatures are rejected.
- Member declaration order does not affect declaration availability.
- Import paths are identifier sequences rather than string literals.
- Array types are one-dimensional; repeated `[]` suffixes are rejected.
- `?` may qualify a reference type or an array reference independently from
  its element type. Nullable primitives, enums, structs, and `void?` are rejected
  semantically; nullable enum arrays remain valid references.
- A `for` iteration declaration uses either `var` inference or an explicit
  element type.
- A `var` local requires an initializer. A final local also requires an
  initializer.
- A final field is initialized by its declaration or exactly once on every
  constructor exit path.
- A non-null reference field is initialized by its declaration or on every
  constructor exit path. Constructor initialization must be a direct assignment
  to the current instance.
- `self` cannot escape, and instance functions cannot be called on the current
  object, until all non-null reference fields are initialized.
- Direct `null` comparisons narrow nullable locals and parameters on proven
  branches. Assignment invalidates a narrowing; fields are not narrowed.
- Nullable conditions test presence. `?.`, `??`, and postfix `!` provide safe
  access, lazy fallback, and checked non-null assertion respectively.
- `expression::name` performs a language-defined meta query. Meta names are
  case-sensitive, do not participate in member visibility, and are resolved
  from the expression's type.
- `value is T` requires a non-null runtime-checkable target. `value as T?`
  requires a nullable target and yields `null` when the runtime type differs.
- A static field must also be final, must have an eligible scalar constant
  initializer, and is evaluated during compilation.
- `IntegerType::wrap(value)` and `IntegerType::sat(value)` require one
  non-nullable integer operand. They preserve the operand's independently
  inferred type, return the named target type, and remain distinct from checked
  `NumericType(value)` conversion.
- `Main` must be declared `static`.

Type checking, assignment-target validation, return checking, and overload
resolution are defined in [semantic_analysis.md](semantic_analysis.md).
Package discovery and import binding are defined in
[packages_and_imports.md](packages_and_imports.md).
