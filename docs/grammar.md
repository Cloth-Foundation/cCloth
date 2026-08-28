# Implemented Cloth grammar

This document defines the syntax implemented through Stage 16.5.
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
      ( explicit_file_class | { member_declaration } ) ;

explicit_file_class
    = "class" [ ":" named_type ]
      "{" { member_declaration } "}" ;

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
      block ;

function_modifier
    = "static"
    | "override" ;

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

At file scope, `class` opens the optional envelope for the implicit file class;
the file name is never repeated. Inside that envelope or after an unwrapped
member, `class` remains a reserved nested-type declaration starter. `struct`
and `enum` are also reserved for future type declarations. These nested forms
are diagnosed as unsupported.

`void` is accepted only as a function return type. An omitted function return
type defaults to `void`. Fields, parameters, locals, arrays, and iteration
bindings require a value-producing `type`.

Imports must precede the explicit class declaration or every unwrapped member
declaration. An explicit class body consumes the remainder of the file. A
`module` declaration is not part of Cloth: the source path relative to the
project source root supplies the package identity.

Function modifiers may appear in either order but may not be repeated.
`override` is valid only on a public instance function that exactly matches an
inherited name and canonical parameter signature. It cannot be combined with
`static`; semantic analysis enforces the complete override contract.

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
    | break_statement
    | continue_statement
    | expression_statement
    | block ;

local_variable_statement
    = [ "final" ] ( type | "var" ) identifier
      [ "=" expression ] ";" ;

return_statement
    = "return" [ expression ] ";" ;

if_statement
    = "if" "(" expression ")" block [ "else" block ] ;

while_statement
    = "while" "(" expression ")" block ;

for_statement
    = "for" "(" iteration_declaration "in" expression ")" block ;

iteration_declaration
    = [ "final" ] ( "var" identifier | type identifier ) ;

break_statement
    = "break" ";" ;

continue_statement
    = "continue" ";" ;

expression_statement
    = expression ";" ;
```

Braces and semicolons shown above are mandatory. `break` and `continue` are
valid only inside a `while` or `for` body. Nested type declarations inside
blocks remain deferred.

## Expressions

```ebnf
expression
    = assignment_expression ;

assignment_expression
    = null_coalescing_expression [ "=" assignment_expression ] ;

null_coalescing_expression
    = logical_or_expression [ "??" null_coalescing_expression ] ;

logical_or_expression
    = logical_and_expression { "||" logical_and_expression } ;

logical_and_expression
    = equality_expression { "&&" equality_expression } ;

equality_expression
    = comparison_expression
      { ( "==" | "!=" ) comparison_expression } ;

comparison_expression
    = additive_expression
      { ( "<" | "<=" | ">" | ">=" ) additive_expression
      | "is" type
      | "as" type } ;

additive_expression
    = multiplicative_expression
      { ( "+" | "-" ) multiplicative_expression } ;

multiplicative_expression
    = unary_expression
      { ( "*" | "/" | "%" ) unary_expression } ;

unary_expression
    = ( "!" | "+" | "-" | "~" ) unary_expression
    | postfix_expression ;

postfix_expression
    = primary_expression
      { call_suffix | member_suffix | meta_suffix | safe_member_suffix | index_suffix
      | "!" } ;

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
    | integer_literal
    | float_literal
    | string_literal
    | character_literal
    | "true"
    | "false"
    | "null"
    | array_literal
    | "(" expression ")" ;
```

The precedence table, from lowest to highest, is:

| Precedence | Operators                              | Associativity |
|-----------:|----------------------------------------|---------------|
| 1          | `=`                                    | right         |
| 2          | `??`                                   | right         |
| 3          | `||`                                   | left          |
| 4          | `&&`                                   | left          |
| 5          | `==`, `!=`                             | left          |
| 6          | `<`, `<=`, `>`, `>=`, `is T`, `as T`  | left          |
| 7          | `+`, `-`                               | left          |
| 8          | `*`, `/`, `%`                          | left          |
| 9          | prefix `!`, `+`, `-`, `~`              | right         |
| 10         | calls, members, meta queries, indexing, postfix `!` | left |

Stage 1.0 deliberately defers compound assignment, increment/decrement,
bitwise binary operators, and shifts even though the lexer recognizes them.

## Contextual constraints

The declaration pass enforces these rules separately from the grammar:

- The source file stem must be a valid Cloth identifier.
- An explicit `class` declaration never repeats the implicit file-class name.
- A base clause names at most one visible file class. The inheritance graph
  cannot contain direct or indirect cycles.
- Member lookup uses the nearest class in the base chain that declares a name.
  Private declarations remain visible only in their owning file class.
- Derived references implicitly widen to direct or transitive base types,
  including compatible nullable forms. The reverse conversion requires `as`.
- A constructor name must exactly match the implicit file-class name.
- Every declared constructor in a derived file class must include a base
  initializer naming its direct base. Root constructors cannot include one.
- Base-initializer arguments use ordinary constructor overload selection and
  cannot access `self`, instance fields, or unqualified instance functions.
- Declaration visibility is inferred from the first ASCII character.
- Constructors inherit file-class visibility.
- Conflicting fields and exact duplicate callable signatures are rejected.
- Member declaration order does not affect declaration availability.
- Import paths are identifier sequences rather than string literals.
- Array types are one-dimensional; repeated `[]` suffixes are rejected.
- `?` may qualify a reference type or an array reference independently from
  its element type. Nullable primitives and `void?` are rejected semantically.
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
- A static field must also be final, must have an initializer, and currently
  accepts only a scalar literal initializer.
- `Main` must be declared `static`.

Type checking, assignment-target validation, return checking, and overload
resolution are defined in [semantic_analysis.md](semantic_analysis.md).
Package discovery and import binding are defined in
[packages_and_imports.md](packages_and_imports.md).
