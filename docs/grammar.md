# Cloth Stage 1.0 grammar

This document defines only the syntax implemented by the Stage 1.0 parser.
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
    = { member_declaration } ;

member_declaration
    = field_declaration
    | function_declaration
    | constructor_declaration ;

field_declaration
    = type identifier [ "=" expression ] ";" ;

function_declaration
    = "function" identifier
      "(" [ parameter_list ] ")"
      [ ":" type ]
      block ;

constructor_declaration
    = identifier
      "(" [ parameter_list ] ")"
      block ;

parameter_list
    = parameter { "," parameter } ;

parameter
    = type identifier ;

type
    = primitive_type
    | named_type ;

primitive_type
    = "bool" | "byte" | "char"
    | "int" | "int8" | "int16" | "int32" | "int64"
    | "uint" | "uint8" | "uint16" | "uint32" | "uint64"
    | "float32" | "float64" ;

named_type
    = identifier ;
```

`struct`, `class`, and `enum` are reserved as possible nested-type declaration
starters, but Stage 1.0 diagnoses them as unsupported.

## Statements

```ebnf
block
    = "{" { statement } "}" ;

statement
    = local_variable_statement
    | return_statement
    | if_statement
    | expression_statement
    | block ;

local_variable_statement
    = type identifier [ "=" expression ] ";" ;

return_statement
    = "return" [ expression ] ";" ;

if_statement
    = "if" "(" expression ")" block [ "else" block ] ;

expression_statement
    = expression ";" ;
```

Braces and semicolons shown above are mandatory. Stage 1.0 does not implement
loops or declarations inside blocks.

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
      { call_suffix | member_suffix } ;

call_suffix
    = "(" [ argument_list ] ")" ;

argument_list
    = expression { "," expression } ;

member_suffix
    = "." identifier ;

primary_expression
    = identifier
    | integer_literal
    | float_literal
    | string_literal
    | character_literal
    | "true"
    | "false"
    | "null"
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
| 9          | calls and member access  | left          |

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

Module lookup, type checking, assignment-target validation, return checking, and
overload resolution belong to later semantic stages.
