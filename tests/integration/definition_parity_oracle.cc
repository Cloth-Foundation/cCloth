// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/parser/parser.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#include "parse_diagnostic_records.h"

namespace {

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

void write_flag(bool value) { std::cout << (value ? 1 : 0); }

void write_range(const cloth::SourceRange& value) {
  std::cout << value.begin.byte_offset << ',' << value.begin.line << ','
            << value.begin.column << ',' << value.end.byte_offset << ','
            << value.end.line << ',' << value.end.column;
}

void write_bytes(std::string_view value) {
  if (value.empty()) {
    std::cout << '-';
    return;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << static_cast<unsigned int>(
        static_cast<unsigned char>(value[index]));
  }
}

int visibility_code(cloth::Visibility value) {
  return value == cloth::Visibility::kPrivate ? 0 : 1;
}

class DefinitionWriter {
 public:
  explicit DefinitionWriter(const cloth::FileClassDecl& file) : file_(file) {}

  void write() {
    std::cout << "F|";
    write_flag(file_.is_valid);
    std::cout << '|' << static_cast<int>(file_.kind) << '|';
    write_flag(file_.has_explicit_class_declaration);
    std::cout << '|';
    write_flag(file_.is_abstract);
    std::cout << '|';
    write_flag(file_.is_sealed);
    std::cout << '|';
    write_range(file_.range);
    std::cout << '\n';
    if (file_.base_class) write_type("base", *file_.base_class);
    for (const cloth::TypeSyntax& value : file_.interfaces) {
      write_type("interface", value);
    }
    for (std::size_t index = 0; index < file_.member_order.size(); ++index) {
      write_declaration(index, file_.member_order[index]);
    }
  }

 private:
  void write_type(std::string_view role, const cloth::TypeSyntax& value) {
    std::cout << "T|" << role << '|';
    write_flag(value.is_primitive);
    std::cout << '|';
    write_flag(value.is_array);
    std::cout << '|';
    write_flag(value.is_nullable);
    std::cout << '|';
    write_flag(value.is_element_nullable);
    std::cout << '|';
    write_range(value.range);
    std::cout << '|';
    write_bytes(value.name);
    std::cout << '\n';
  }

  void write_declaration(std::size_t index,
                         const cloth::MemberReference& reference) {
    if (reference.kind == cloth::DeclarationKind::kField) {
      const cloth::FieldDecl& value = file_.fields[reference.index];
      std::cout << "D|" << index << "|0|";
      write_flag(value.is_valid);
      std::cout << '|' << visibility_code(value.visibility) << '|';
      write_flag(value.is_final);
      std::cout << '|';
      write_flag(value.is_static);
      std::cout << '|';
      write_range(value.range);
      std::cout << '|';
      write_bytes(value.name);
      std::cout << '\n';
      write_type("declared", value.type);
      if (value.initializer) write_expression(*value.initializer);
      return;
    }
    if (reference.kind == cloth::DeclarationKind::kFunction) {
      const cloth::FunctionDecl& value = file_.functions[reference.index];
      std::cout << "D|" << index << "|1|";
      write_flag(value.is_valid);
      std::cout << '|' << visibility_code(value.visibility) << '|';
      write_flag(value.is_static);
      std::cout << '|';
      write_flag(value.is_override);
      std::cout << '|';
      write_flag(value.is_abstract);
      std::cout << '|';
      write_flag(value.is_final);
      std::cout << '|';
      write_flag(value.has_explicit_throws);
      std::cout << '|';
      write_range(value.range);
      std::cout << '|';
      write_bytes(value.name);
      std::cout << '\n';
      write_parameters(value.parameters);
      if (value.return_type) write_type("return", *value.return_type);
      for (const cloth::TypeSyntax& type : value.throws_types) {
        write_type("throws", type);
      }
      write_block(value.body);
      return;
    }
    const cloth::ConstructorDecl& value = file_.constructors[reference.index];
    std::cout << "D|" << index << "|2|";
    write_flag(value.is_valid);
    std::cout << '|' << visibility_code(value.visibility) << '|';
    write_flag(value.has_explicit_throws);
    std::cout << '|';
    write_range(value.range);
    std::cout << '|';
    write_bytes(value.name);
    std::cout << '\n';
    write_parameters(value.parameters);
    for (const cloth::TypeSyntax& type : value.throws_types) {
      write_type("throws", type);
    }
    if (value.initializer) {
      std::cout << "C|";
      write_flag(value.initializer->is_valid);
      std::cout << '|';
      write_range(value.initializer->range);
      std::cout << '|' << value.initializer->arguments.size() << '\n';
      write_type("constructor-base", value.initializer->base_type);
      for (cloth::ExpressionId argument : value.initializer->arguments) {
        write_expression(argument);
      }
    }
    write_block(value.body);
  }

  template <typename Parameters>
  void write_parameters(const Parameters& values) {
    for (const cloth::ParameterDecl& value : values) {
      std::cout << "P|";
      write_flag(value.is_final);
      std::cout << '|';
      write_range(value.range);
      std::cout << '|';
      write_bytes(value.name);
      std::cout << '\n';
      write_type("parameter", value.type);
    }
  }

  void write_block(cloth::BlockId id) {
    const cloth::Block& value = file_.storage.block(id);
    std::cout << "B|" << next_block_++ << '|';
    write_flag(value.is_valid);
    std::cout << '|';
    write_range(value.range);
    std::cout << '|' << value.statements.size() << '\n';
    for (cloth::StatementId statement : value.statements) {
      write_statement(statement);
    }
  }

  void write_statement(cloth::StatementId id) {
    const cloth::Statement& value = file_.storage.statement(id);
    const std::size_t kind = value.data.index();
    std::cout << "S|" << next_statement_++ << '|' << kind << '|';
    write_flag(kind != 0);
    std::cout << '|';
    write_range(value.range);
    std::cout << '\n';
    std::visit(Overloaded{[](const cloth::InvalidStatement&) {},
                          [this](const cloth::LocalVariableStatement& node) {
                            if (node.type) write_type("local", *node.type);
                            std::cout << "V|";
                            write_flag(node.is_final);
                            std::cout << '|';
                            write_bytes(node.name);
                            std::cout << '\n';
                            if (node.initializer)
                              write_expression(*node.initializer);
                          },
                          [this](const cloth::ReturnStatement& node) {
                            if (node.value) write_expression(*node.value);
                          },
                          [this](const cloth::ExpressionStatement& node) {
                            write_expression(node.expression);
                          },
                          [this](const cloth::IfStatement& node) {
                            write_expression(node.condition);
                            write_block(node.then_block);
                            if (node.else_block) write_block(*node.else_block);
                          },
                          [this](const cloth::WhileStatement& node) {
                            write_expression(node.condition);
                            write_block(node.body);
                          },
                          [this](const cloth::ForEachStatement& node) {
                            if (node.variable.type) {
                              write_type("for-variable", *node.variable.type);
                            }
                            std::cout << "V|";
                            write_flag(node.variable.is_final);
                            std::cout << '|';
                            write_range(node.variable.range);
                            std::cout << '|';
                            write_bytes(node.variable.name);
                            std::cout << '\n';
                            write_expression(node.iterable);
                            write_block(node.body);
                          },
                          [this](const cloth::ForStatement& node) {
                            if (node.initializer)
                              write_statement(*node.initializer);
                            if (node.condition)
                              write_expression(*node.condition);
                            for (cloth::ExpressionId update : node.updates) {
                              write_expression(update);
                            }
                            write_block(node.body);
                          },
                          [this](const cloth::SwitchStatement& node) {
                            write_expression(node.selector);
                            for (const cloth::SwitchArm& arm : node.arms) {
                              std::cout << "A|";
                              write_range(arm.range);
                              std::cout << '|' << arm.labels.size() << '\n';
                              for (cloth::ExpressionId label : arm.labels) {
                                write_expression(label);
                              }
                              write_block(arm.body);
                            }
                          },
                          [](const cloth::BreakStatement&) {},
                          [](const cloth::ContinueStatement&) {},
                          [this](const cloth::NestedBlockStatement& node) {
                            write_block(node.block);
                          }},
               value.data);
  }

  void write_expression(cloth::ExpressionId id) {
    const cloth::Expression& value = file_.storage.expression(id);
    const std::size_t kind = value.data.index();
    std::cout << "X|" << next_expression_++ << '|' << kind << '|';
    write_flag(kind != 0);
    std::cout << '|';
    write_range(value.range);
    std::cout << '\n';
    std::visit(
        Overloaded{
            [](const cloth::InvalidExpression&) {},
            [](const cloth::IdentifierExpression& node) {
              std::cout << "N|";
              write_bytes(node.name);
              std::cout << '\n';
            },
            [](const cloth::LiteralExpression& node) {
              std::cout << "L|" << static_cast<int>(node.kind) << '|';
              write_bytes(node.lexeme);
              std::cout << '\n';
            },
            [](const cloth::SuperExpression&) {},
            [this](const cloth::ThrowExpression& node) {
              write_expression(node.operand);
            },
            [this](const cloth::UnaryExpression& node) {
              std::cout << "O|" << static_cast<int>(node.operation) << '\n';
              write_expression(node.operand);
            },
            [this](const cloth::UpdateExpression& node) {
              std::cout << "O|" << static_cast<int>(node.operation) << '|';
              write_flag(node.is_postfix);
              std::cout << '\n';
              write_expression(node.operand);
            },
            [this](const cloth::BinaryExpression& node) {
              std::cout << "O|" << static_cast<int>(node.operation) << '\n';
              write_expression(node.left);
              write_expression(node.right);
            },
            [this](const cloth::TypeTestExpression& node) {
              write_type("target", node.target);
              write_expression(node.value);
            },
            [this](const cloth::CheckedCastExpression& node) {
              write_type("target", node.target);
              write_expression(node.value);
            },
            [this](const cloth::NumericConversionExpression& node) {
              write_type("target", node.target);
              write_expression(node.value);
            },
            [this](const cloth::IntegerConversionExpression& node) {
              write_type("target", node.target);
              std::cout << "N|";
              write_bytes(node.operation);
              std::cout << '\n';
              write_expression(node.value);
            },
            [this](const cloth::AssignmentExpression& node) {
              std::cout << "O|" << static_cast<int>(node.operation) << '\n';
              write_expression(node.target);
              write_expression(node.value);
            },
            [this](const cloth::MemberAccessExpression& node) {
              write_name_and_receiver(node.member, node.object);
            },
            [this](const cloth::MetaAccessExpression& node) {
              write_name_and_receiver(node.meta, node.object);
            },
            [this](const cloth::SafeMemberAccessExpression& node) {
              write_name_and_receiver(node.member, node.object);
            },
            [this](const cloth::SafeMetaAccessExpression& node) {
              write_name_and_receiver(node.meta, node.object);
            },
            [this](const cloth::NullCoalesceExpression& node) {
              write_expression(node.nullable);
              write_expression(node.fallback);
            },
            [this](const cloth::NullAssertExpression& node) {
              write_expression(node.operand);
            },
            [this](const cloth::CallExpression& node) {
              std::cout << "Q|" << node.arguments.size() << '\n';
              write_expression(node.callee);
              for (cloth::ExpressionId argument : node.arguments) {
                write_expression(argument);
              }
            },
            [this](const cloth::ArrayLiteralExpression& node) {
              std::cout << "Q|" << node.elements.size() << '\n';
              for (cloth::ExpressionId element : node.elements) {
                write_expression(element);
              }
            },
            [this](const cloth::ArrayConstructionExpression& node) {
              write_type("element", node.element_type);
              write_expression(node.length);
            },
            [this](const cloth::IndexExpression& node) {
              write_expression(node.object);
              write_expression(node.index);
            },
            [this](const cloth::ParenthesizedExpression& node) {
              write_expression(node.expression);
            }},
        value.data);
  }

  void write_name_and_receiver(std::string_view name,
                               cloth::ExpressionId receiver) {
    std::cout << "N|";
    write_bytes(name);
    std::cout << '\n';
    write_expression(receiver);
  }

  const cloth::FileClassDecl& file_;
  std::size_t next_expression_{0};
  std::size_t next_statement_{0};
  std::size_t next_block_{0};
};

int write_records(const std::filesystem::path& path) {
  auto loaded = cloth::SourceFile::load(path);
  if (!loaded) {
    std::cerr << path.string() << ": " << loaded.error().message << '\n';
    return 1;
  }
  cloth::SourceFile source = std::move(*loaded);
  cloth::DiagnosticEngine diagnostics;
  const auto tokens = cloth::Lexer{source, diagnostics}.lex();
  if (diagnostics.has_errors()) {
    std::cerr << "definition record input has lexical diagnostics\n";
    return 1;
  }
  cloth::ParseResult result =
      cloth::Parser{source, tokens, diagnostics}.parse();
  DefinitionWriter{result.file_class}.write();
  return cloth::test::write_parse_diagnostic_records(
             diagnostics.diagnostics(), "G", std::cout, std::cerr, "definition")
             ? 0
             : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) return write_records(argv[1]);
  std::cerr << "usage: cloth_definition_parity_oracle <path>\n";
  return 2;
}
