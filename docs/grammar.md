# Implemented Cloth grammar

This document defines the syntax implemented through Stage 11.
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
    = { import_declaration } { member_declaration } ;

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
    = type identifier [ "=" expression ] ";" ;

function_declaration
    = "func" identifier
      "(" [ parameter_list ] ")"
      [ ":" return_type ]
      block ;

return_type
    = type
    | "void" ;

constructor_declaration
    = identifier
      "(" [ parameter_list ] ")"
      block ;

parameter_list
    = parameter { "," parameter } ;

parameter
    = type identifier ;

type
    = element_type [ "[" "]" ] ;

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

`struct`, `class`, and `enum` are reserved as possible nested-type declaration
starters, but Stage 1.0 diagnoses them as unsupported.

`void` is accepted only as a function return type. An omitted function return
type defaults to `void`. Fields, parameters, locals, arrays, and iteration
bindings require a value-producing `type`.

Imports must precede every member declaration. A `module` declaration is not
part of Cloth: the source path relative to the project source root supplies the
package identity.

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
    = type identifier [ "=" expression ] ";" ;

return_statement
    = "return" [ expression ] ";" ;

if_statement
    = "if" "(" expression ")" block [ "else" block ] ;

while_statement
    = "while" "(" expression ")" block ;

for_statement
    = "for" "(" iteration_declaration "in" expression ")" block ;

iteration_declaration
    = "var" identifier
    | type identifier ;

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
    = logical_or_expression [ "=" assignment_expression ] ;

logical_or_expression
    = logical_and_expression { "||" logical_and_expression } ;

logical_and_expression
    = equality_expression { "&&" equality_expression } ;

equality_expression
    = comparison_expression
      { ( "==" | "!=" ) comparison_expression } ;

comparison_expression
    = additive_expression
      { ( "<" | "<=" | ">" | ">=" ) additive_expression } ;

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
      { call_suffix | member_suffix | index_suffix } ;

call_suffix
    = "(" [ argument_list ] ")" ;

argument_list
    = expression { "," expression } ;

member_suffix
    = "." identifier ;

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

| Precedence | Operators                | Associativity |
|-----------:|--------------------------|---------------|
| 1          | `=`                      | right         |
| 2          | `||`                     | left          |
| 3          | `&&`                     | left          |
| 4          | `==`, `!=`               | left          |
| 5          | `<`, `<=`, `>`, `>=`     | left          |
| 6          | `+`, `-`                 | left          |
| 7          | `*`, `/`, `%`            | left          |
| 8          | prefix `!`, `+`, `-`, `~`| right         |
| 9          | calls, members, indexing | left          |

Stage 1.0 deliberately defers compound assignment, increment/decrement,
bitwise binary operators, and shifts even though the lexer recognizes them.

## Contextual constraints

The declaration pass enforces these rules separately from the grammar:

- The source file stem must be a valid Cloth identifier.
- A constructor name must exactly match the implicit file-class name.
- Declaration visibility is inferred from the first ASCII character.
- Constructors inherit file-class visibility.
- Conflicting fields and exact duplicate callable signatures are rejected.
- Member declaration order does not affect declaration availability.
- Import paths are identifier sequences rather than string literals.
- Array types are one-dimensional; repeated `[]` suffixes are rejected.
- A `for` iteration declaration uses either `var` inference or an explicit
  element type.

Type checking, assignment-target validation, return checking, and overload
resolution are defined in [semantic_analysis.md](semantic_analysis.md).
Package discovery and import binding are defined in
[packages_and_imports.md](packages_and_imports.md).
