#include "cloth/ast/ast.h"
#include "cloth/ast/ast_printer.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/parser/parser.h"
#include "cloth/sema/file_class_symbols.h"
#include "cloth/sema/visibility.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

class TestContext {
 public:
  explicit TestContext(std::string_view name) : name_(name) {}

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "  " << name_ << ": " << message << '\n';
    }
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  std::string_view name_;
  int failures_{0};
};

struct ParsedSource {
  cloth::SourceFile source;
  cloth::DiagnosticEngine diagnostics;
  std::vector<cloth::Token> tokens;
  std::optional<cloth::ParseResult> result;

  ParsedSource(std::filesystem::path path, std::string text)
      : source(
            cloth::SourceFile::from_memory(std::move(path), std::move(text))) {
    tokens = cloth::Lexer{source, diagnostics}.lex();
    result.emplace(cloth::Parser{source, tokens, diagnostics}.parse());
  }

  [[nodiscard]] const cloth::FileClassDecl& ast() const {
    return result->file_class;
  }

  [[nodiscard]] const cloth::FileClassSymbols& symbols() const {
    return result->symbols;
  }
};

std::size_t error_count(const ParsedSource& source) {
  std::size_t count = 0;
  for (const cloth::Diagnostic& diagnostic : source.diagnostics.diagnostics()) {
    if (diagnostic.severity == cloth::DiagnosticSeverity::kError) {
      ++count;
    }
  }
  return count;
}

bool has_diagnostic(const ParsedSource& source, std::string_view text) {
  for (const cloth::Diagnostic& diagnostic : source.diagnostics.diagnostics()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool has_member(const ParsedSource& source, std::string_view name,
                cloth::DeclarationKind kind) {
  for (const cloth::MemberSymbol& member : source.symbols().members()) {
    if (member.name == name && member.kind == kind) {
      return true;
    }
  }
  return false;
}

void empty_source(TestContext& test) {
  const ParsedSource source{"Empty.co", ""};
  test.expect(error_count(source) == 0, "empty source should parse");
  test.expect(source.ast().name == "Empty", "wrong implicit class name");
  test.expect(source.ast().visibility == cloth::Visibility::kPublic,
              "wrong implicit class visibility");
  test.expect(source.ast().member_order.empty(), "empty class has members");
  test.expect(source.ast().range.begin.byte_offset == 0 &&
                  source.ast().range.end.byte_offset == 0,
              "empty class range is wrong");
}

void parser_requires_eof(TestContext& test) {
  cloth::SourceFile source = cloth::SourceFile::from_memory("Invariant.co", "");
  cloth::DiagnosticEngine diagnostics;
  const std::vector<cloth::Token> tokens;
  const cloth::ParseResult result =
      cloth::Parser{source, tokens, diagnostics}.parse();
  test.expect(!result.is_valid,
              "parser accepted a token stream without an eof token");
  test.expect(diagnostics.has_errors(),
              "invalid parser input did not produce a diagnostic");
}

void fields_and_visibility(TestContext& test) {
  const ParsedSource source{"Fields.co",
                            "String Name;\nint32 id;\nbool active = true;\n"
                            "int32 _cache;\n"};
  test.expect(error_count(source) == 0, "valid fields should parse");
  test.expect(source.ast().fields.size() == 4, "wrong field count");
  if (source.ast().fields.size() != 4) {
    return;
  }
  test.expect(
      source.ast().fields[0].name == "Name" &&
          source.ast().fields[0].visibility == cloth::Visibility::kPublic,
      "public field inference failed");
  test.expect(
      source.ast().fields[1].name == "id" &&
          source.ast().fields[1].visibility == cloth::Visibility::kPrivate,
      "private field inference failed");
  test.expect(source.ast().fields[2].initializer.has_value(),
              "initialized field lost its expression");
  if (source.ast().fields[2].initializer) {
    const cloth::Expression& expression =
        source.ast().storage.expression(*source.ast().fields[2].initializer);
    const auto* literal =
        std::get_if<cloth::LiteralExpression>(&expression.data);
    test.expect(
        literal != nullptr && literal->kind == cloth::LiteralKind::kBoolean,
        "field initializer is not a boolean literal");
  }
  test.expect(source.ast().fields[3].visibility == cloth::Visibility::kPrivate,
              "underscore field should be private");

  const ParsedSource private_class{"user.co", ""};
  test.expect(private_class.ast().visibility == cloth::Visibility::kPrivate,
              "lowercase file class should be private");
}

void functions(TestContext& test) {
  const ParsedSource source{
      "Functions.co",
      "function shutdown() {}\n"
      "function Add(int a, int b): int { return a + b; }\n"};
  test.expect(error_count(source) == 0, "valid functions should parse");
  test.expect(source.ast().functions.size() == 2, "wrong function count");
  if (source.ast().functions.size() != 2) {
    return;
  }
  const cloth::FunctionDecl& shutdown = source.ast().functions[0];
  test.expect(shutdown.parameters.empty(), "no-parameter function is wrong");
  test.expect(!shutdown.return_type.has_value(),
              "omitted return type should stay absent");
  test.expect(shutdown.visibility == cloth::Visibility::kPrivate,
              "lowercase function should be private");

  const cloth::FunctionDecl& add = source.ast().functions[1];
  test.expect(add.parameters.size() == 2, "function parameters were lost");
  test.expect(add.return_type && add.return_type->name == "int",
              "function return type is wrong");
  test.expect(add.visibility == cloth::Visibility::kPublic,
              "uppercase function should be public");
  test.expect(source.ast().storage.block(add.body).statements.size() == 1,
              "function body was not parsed");
}

void constructors(TestContext& test) {
  const ParsedSource source{
      "User.co", "User() {}\nUser(String name) { self.Name = name; }\n"};
  test.expect(error_count(source) == 0, "valid constructors should parse");
  test.expect(source.ast().constructors.size() == 2,
              "multiple constructors were not retained");
  if (source.ast().constructors.size() == 2) {
    test.expect(
        source.ast().constructors[0].visibility == cloth::Visibility::kPublic &&
            source.ast().constructors[1].visibility ==
                cloth::Visibility::kPublic,
        "constructor visibility should come from the file class");
  }
}

void wrong_constructor(TestContext& test) {
  const ParsedSource source{"User.co", "Person(String name) {}\n"};
  test.expect(has_diagnostic(source, "must match implicit class 'User'"),
              "wrong constructor name was not diagnosed");
  test.expect(source.ast().constructors.size() == 1,
              "recovered constructor should remain in the AST");
  if (!source.ast().constructors.empty()) {
    test.expect(!source.ast().constructors[0].is_valid,
                "wrong constructor should be marked invalid");
  }
}

void invalid_file_class_name(TestContext& test) {
  const ParsedSource source{"123-User.co", "int32 id;\n"};
  test.expect(has_diagnostic(source, "not a valid Cloth identifier"),
              "invalid file stem was not diagnosed");
  test.expect(!source.result->is_valid, "invalid file class parsed as valid");
}

void duplicate_declarations(TestContext& test) {
  const ParsedSource source{
      "Duplicates.co",
      "int32 id;\nint32 id;\n"
      "function Find(int x) {}\nfunction Find(int y) {}\n"};
  test.expect(has_diagnostic(source, "conflicts with previous field"),
              "duplicate field was not diagnosed");
  test.expect(has_diagnostic(source, "duplicate function signature"),
              "duplicate function signature was not diagnosed");
  test.expect(source.symbols().members().size() == 4,
              "duplicate declarations should remain inspectable");
}

void overload_candidates(TestContext& test) {
  const ParsedSource source{"Overloads.co",
                            "function Find(int value) {}\n"
                            "function Find(String value) {}\n"};
  test.expect(error_count(source) == 0,
              "distinct signatures should remain overload candidates");
  test.expect(source.ast().functions.size() == 2,
              "overload candidates were not retained");
}

void declaration_order_independence(TestContext& test) {
  const ParsedSource first{"Order.co",
                           "function A(): int { return B(); }\n"
                           "function B(): int { return 10; }\n"};
  const ParsedSource second{"Order.co",
                            "function B(): int { return 10; }\n"
                            "function A(): int { return B(); }\n"};
  test.expect(error_count(first) == 0 && error_count(second) == 0,
              "declaration order changed validity");
  for (const std::string_view name :
       {std::string_view{"A"}, std::string_view{"B"}}) {
    test.expect(has_member(first, name, cloth::DeclarationKind::kFunction) &&
                    has_member(second, name, cloth::DeclarationKind::kFunction),
                "declaration order changed symbol discovery");
  }
}

void malformed_parameters_recover(TestContext& test) {
  const ParsedSource source{"Parameters.co",
                            "function Broken(int a int b): int { return 0; }\n"
                            "function Valid(): int { return 10; }\n"};
  test.expect(has_diagnostic(source, "expected ',' or ')' after parameter"),
              "missing parameter comma was not diagnosed");
  test.expect(has_member(source, "Valid", cloth::DeclarationKind::kFunction),
              "parser did not recover to the valid function");
  test.expect(
      !source.ast().functions.empty() && !source.ast().functions[0].is_valid,
      "malformed function should be marked invalid");
}

void missing_parenthesis_recover(TestContext& test) {
  const ParsedSource source{"Parenthesis.co",
                            "function Broken(int value { return value; }\n"
                            "function Valid() {}\n"};
  test.expect(has_diagnostic(source, "expected ')' after parameter list"),
              "missing parenthesis was not diagnosed");
  test.expect(has_member(source, "Valid", cloth::DeclarationKind::kFunction),
              "missing parenthesis prevented declaration recovery");
}

void missing_function_brace_recover(TestContext& test) {
  const ParsedSource source{"Brace.co",
                            "function Broken(): int;\nfunction Valid() {}\n"};
  test.expect(has_diagnostic(source, "expected '{' to begin body"),
              "missing function brace was not diagnosed");
  test.expect(has_member(source, "Valid", cloth::DeclarationKind::kFunction),
              "missing function brace prevented recovery");
}

void malformed_field_recover(TestContext& test) {
  const ParsedSource source{
      "Recovery.co", "int32 broken =\nfunction Valid(): int { return 10; }\n"};
  test.expect(has_diagnostic(source, "expected expression after '='"),
              "missing field initializer was not diagnosed");
  test.expect(has_diagnostic(source, "expected ';' after field declaration"),
              "missing field semicolon was not diagnosed");
  test.expect(has_member(source, "Valid", cloth::DeclarationKind::kFunction),
              "field recovery lost the following function");
  test.expect(!source.ast().fields.empty() && !source.ast().fields[0].is_valid,
              "malformed field should be marked invalid");
}

void unexpected_top_level_token(TestContext& test) {
  const ParsedSource source{"Unexpected.co", "while;\nint32 valid;\n"};
  test.expect(
      has_diagnostic(source, "expected a field, function, or constructor"),
      "unexpected token was not diagnosed");
  test.expect(has_member(source, "valid", cloth::DeclarationKind::kField),
              "unexpected token recovery lost the following field");
}

void deferred_nested_type_recover(TestContext& test) {
  const ParsedSource source{"Nested.co", "struct Inner {}\nint32 valid;\n"};
  test.expect(has_diagnostic(source, "nested type declarations are reserved"),
              "deferred nested type was not diagnosed explicitly");
  test.expect(has_member(source, "valid", cloth::DeclarationKind::kField),
              "nested-type recovery lost the following field");
}

void expressions_and_if_statement(TestContext& test) {
  const ParsedSource source{
      "Expressions.co",
      "function Check(int x): bool {\n"
      "  int value = x + 1 * 2;\n"
      "  if (value > 0) { return true; } else { return false; }\n"
      "}\n"};
  test.expect(error_count(source) == 0,
              "valid statements and expressions should parse");
  if (source.ast().functions.empty()) {
    test.expect(false, "missing Check function");
    return;
  }
  const cloth::Block& body =
      source.ast().storage.block(source.ast().functions[0].body);
  test.expect(body.statements.size() == 2, "wrong statement count");
  if (body.statements.size() != 2) {
    return;
  }

  const cloth::Statement& local =
      source.ast().storage.statement(body.statements[0]);
  const auto* local_data =
      std::get_if<cloth::LocalVariableStatement>(&local.data);
  test.expect(local_data != nullptr && local_data->initializer.has_value(),
              "local variable initializer is missing");
  if (local_data != nullptr && local_data->initializer) {
    const cloth::Expression& addition =
        source.ast().storage.expression(*local_data->initializer);
    const auto* binary = std::get_if<cloth::BinaryExpression>(&addition.data);
    test.expect(
        binary != nullptr && binary->operation == cloth::TokenKind::kPlus,
        "additive expression root has wrong precedence");
    if (binary != nullptr) {
      const cloth::Expression& product =
          source.ast().storage.expression(binary->right);
      const auto* product_data =
          std::get_if<cloth::BinaryExpression>(&product.data);
      test.expect(product_data != nullptr &&
                      product_data->operation == cloth::TokenKind::kStar,
                  "multiplication did not bind more tightly than addition");
    }
  }

  const cloth::Statement& if_statement =
      source.ast().storage.statement(body.statements[1]);
  const auto* if_data = std::get_if<cloth::IfStatement>(&if_statement.data);
  test.expect(if_data != nullptr && if_data->else_block.has_value(),
              "if/else structure was not preserved");
}

void calls_members_and_assignment(TestContext& test) {
  const ParsedSource source{
      "Calls.co",
      "Calls(String name) { self.Name = Repository.Find(name); }\n"};
  test.expect(error_count(source) == 0,
              "member calls and assignments should parse");
  const cloth::Block& body =
      source.ast().storage.block(source.ast().constructors[0].body);
  const cloth::Statement& statement =
      source.ast().storage.statement(body.statements[0]);
  const auto* expression_statement =
      std::get_if<cloth::ExpressionStatement>(&statement.data);
  test.expect(expression_statement != nullptr,
              "assignment should be an expression statement");
  if (expression_statement != nullptr) {
    const cloth::Expression& assignment =
        source.ast().storage.expression(expression_statement->expression);
    test.expect(
        std::holds_alternative<cloth::AssignmentExpression>(assignment.data),
        "assignment AST node is missing");
  }
}

void missing_statement_semicolon_recover(TestContext& test) {
  const ParsedSource source{"Statements.co",
                            "function Values(): int { return 1 return 2; }\n"};
  test.expect(has_diagnostic(source, "expected ';' after return statement"),
              "missing return semicolon was not diagnosed");
  const cloth::Block& body =
      source.ast().storage.block(source.ast().functions[0].body);
  test.expect(body.statements.size() == 2,
              "parser did not recover to the second return");
}

void mandatory_if_braces(TestContext& test) {
  const ParsedSource source{"Braces.co",
                            "function Check() { if (true) return; return; }\n"};
  test.expect(has_diagnostic(source, "expected '{' to begin if body"),
              "brace-less if body was accepted");
}

void malformed_expression_recover(TestContext& test) {
  const ParsedSource source{
      "Malformed.co", "function Broken(): int { return (1 + ); return 2; }\n"};
  test.expect(has_diagnostic(source, "expected expression"),
              "malformed expression was not diagnosed");
  const cloth::Block& body =
      source.ast().storage.block(source.ast().functions[0].body);
  test.expect(body.statements.size() == 2,
              "malformed expression prevented statement recovery");
}

void multiple_independent_errors(TestContext& test) {
  const ParsedSource source{"Errors.co",
                            "int32 broken =\n"
                            "function First(): int { return (1 + ); }\n"
                            "function Second(): int { return 2 }\n"};
  test.expect(error_count(source) >= 4,
              "expected multiple independent diagnostics");
  test.expect(
      has_member(source, "First", cloth::DeclarationKind::kFunction) &&
          has_member(source, "Second", cloth::DeclarationKind::kFunction),
      "recovery did not discover later declarations");
}

void source_ranges(TestContext& test) {
  const std::string text =
      "int32 id;\nfunction Read(int32 value): bool { return true; }\n";
  const ParsedSource source{"Ranges.co", text};
  test.expect(error_count(source) == 0, "range fixture should parse");
  const cloth::FieldDecl& field = source.ast().fields[0];
  test.expect(
      field.range.begin.byte_offset == 0 && field.range.end.byte_offset == 9,
      "field range is not half-open");

  const cloth::FunctionDecl& function = source.ast().functions[0];
  const std::size_t function_begin = text.find("function");
  const std::size_t function_end = text.find('}') + 1;
  test.expect(function.range.begin.byte_offset == function_begin &&
                  function.range.end.byte_offset == function_end,
              "function range is wrong");
  test.expect(function.parameters[0].range.begin.byte_offset ==
                      text.find("int32", function_begin) &&
                  function.parameters[0].range.end.byte_offset ==
                      text.find("value") + std::string_view{"value"}.size(),
              "parameter range is wrong");

  const cloth::Block& block = source.ast().storage.block(function.body);
  test.expect(block.range.begin.byte_offset == text.find('{') &&
                  block.range.end.byte_offset == function_end,
              "block range is wrong");
  const cloth::Statement& statement =
      source.ast().storage.statement(block.statements[0]);
  const auto* return_statement =
      std::get_if<cloth::ReturnStatement>(&statement.data);
  test.expect(return_statement != nullptr && return_statement->value,
              "return expression is missing");
  if (return_statement != nullptr && return_statement->value) {
    const cloth::Expression& expression =
        source.ast().storage.expression(*return_statement->value);
    test.expect(expression.range.begin.byte_offset == text.find("true") &&
                    expression.range.end.byte_offset == text.find("true") + 4,
                "expression range is wrong");
  }

  const std::string leading_text = "\n  int32 value;\n";
  const ParsedSource leading{"Leading.co", leading_text};
  test.expect(leading.ast().range.begin.byte_offset == 0 &&
                  leading.ast().range.begin.line == 1 &&
                  leading.ast().range.begin.column == 1 &&
                  leading.ast().range.end.byte_offset == leading_text.size(),
              "file-class range must cover leading and trailing whitespace");
}

void deterministic_diagnostics(TestContext& test) {
  const std::string text =
      "int32 broken =\n"
      "function Bad(int a int b): int { return (1 + ); }\n";
  const ParsedSource first{"Stable.co", text};
  const ParsedSource second{"Stable.co", text};
  const auto first_diagnostics = first.diagnostics.diagnostics();
  const auto second_diagnostics = second.diagnostics.diagnostics();
  test.expect(first_diagnostics.size() == second_diagnostics.size(),
              "diagnostic count changed between runs");
  const std::size_t count = first_diagnostics.size() < second_diagnostics.size()
                                ? first_diagnostics.size()
                                : second_diagnostics.size();
  for (std::size_t index = 0; index < count; ++index) {
    test.expect(first_diagnostics[index].severity ==
                        second_diagnostics[index].severity &&
                    first_diagnostics[index].message ==
                        second_diagnostics[index].message &&
                    first_diagnostics[index].range.begin.byte_offset ==
                        second_diagnostics[index].range.begin.byte_offset,
                "diagnostic ordering or content changed between runs");
  }
}

void deterministic_ast_summary(TestContext& test) {
  const ParsedSource source{"User.co",
                            "String Name;\nint32 id;\nUser() {}\n"
                            "function Find(int32 id): User { return id; }\n"
                            "function validate(): bool { return true; }\n"};
  std::ostringstream output;
  cloth::print_ast_summary(source.ast(), output);
  const std::string expected =
      "FileClass User [public]\n"
      "|- Field Name: String [public]\n"
      "|- Field id: int32 [private]\n"
      "|- Constructor User() [public]\n"
      "|- Function Find(int32 id): User [public]\n"
      "|- Function validate(): bool [private]\n";
  test.expect(output.str() == expected,
              "AST summary is incomplete or nondeterministic");
}

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"empty source", empty_source},
      {"parser requires eof", parser_requires_eof},
      {"fields and visibility", fields_and_visibility},
      {"functions", functions},
      {"constructors", constructors},
      {"wrong constructor", wrong_constructor},
      {"invalid file class name", invalid_file_class_name},
      {"duplicate declarations", duplicate_declarations},
      {"overload candidates", overload_candidates},
      {"declaration order independence", declaration_order_independence},
      {"malformed parameters recover", malformed_parameters_recover},
      {"missing parenthesis recover", missing_parenthesis_recover},
      {"missing function brace recover", missing_function_brace_recover},
      {"malformed field recover", malformed_field_recover},
      {"unexpected top-level token", unexpected_top_level_token},
      {"deferred nested type recover", deferred_nested_type_recover},
      {"expressions and if statement", expressions_and_if_statement},
      {"calls, members, and assignment", calls_members_and_assignment},
      {"missing statement semicolon recover",
       missing_statement_semicolon_recover},
      {"mandatory if braces", mandatory_if_braces},
      {"malformed expression recover", malformed_expression_recover},
      {"multiple independent errors", multiple_independent_errors},
      {"source ranges", source_ranges},
      {"deterministic diagnostics", deterministic_diagnostics},
      {"deterministic AST summary", deterministic_ast_summary},
  };

  int failures = 0;
  for (const TestCase& test_case : tests) {
    TestContext context{test_case.name};
    test_case.function(context);
    if (context.failures() == 0) {
      std::cout << "[pass] " << test_case.name << '\n';
    } else {
      std::cout << "[fail] " << test_case.name << '\n';
      failures += context.failures();
    }
  }

  if (failures == 0) {
    std::cout << tests.size() << " tests passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}
