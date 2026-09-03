// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_PARSER_DEFINITION_PASS_H_
#define CLOTH_PARSER_DEFINITION_PASS_H_

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/token.h"
#include "cloth/parser/declaration_pass.h"
#include "cloth/sema/file_class_symbols.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace cloth {

class DefinitionPass {
 public:
  DefinitionPass(const SourceFile& source, std::span<const Token> tokens,
                 const FileClassSymbols& symbols,
                 std::span<const ImportDecl> imports,
                 std::span<const MemberOutline> outlines,
                 const std::optional<TypeSyntax>& base_class,
                 bool has_explicit_class_declaration, bool is_abstract,
                 bool is_sealed, FileTypeKind file_type_kind,
                 std::span<const TypeSyntax> interfaces,
                 DiagnosticEngine& diagnostics);

  [[nodiscard]] FileClassDecl run();

 private:
  [[nodiscard]] bool at_limit() const noexcept;
  [[nodiscard]] const Token& current() const noexcept;
  [[nodiscard]] const Token& peek(std::size_t lookahead) const noexcept;
  const Token& advance() noexcept;
  bool match(TokenKind kind) noexcept;
  bool expect(TokenKind kind, std::string_view message);
  [[nodiscard]] bool is_local_variable_start() const noexcept;
  [[nodiscard]] TypeSyntax parse_type();

  void build_field(const MemberOutline& outline, const MemberSymbol& symbol);
  void build_function(const MemberOutline& outline, const MemberSymbol& symbol);
  void build_constructor(const MemberOutline& outline,
                         const MemberSymbol& symbol);
  [[nodiscard]] std::optional<ConstructorInitializer>
  parse_constructor_initializer(TokenIndexRange tokens);
  [[nodiscard]] std::vector<ParameterDecl> copy_parameters(
      const MemberSymbol& symbol) const;

  [[nodiscard]] BlockId parse_block();
  [[nodiscard]] StatementId parse_statement();
  [[nodiscard]] StatementId parse_local_variable();
  [[nodiscard]] StatementId parse_return_statement();
  [[nodiscard]] StatementId parse_if_statement();
  [[nodiscard]] StatementId parse_while_statement();
  [[nodiscard]] StatementId parse_for_statement();
  [[nodiscard]] StatementId parse_switch_statement();
  void synchronize_switch_arm();
  [[nodiscard]] StatementId parse_loop_control_statement();
  [[nodiscard]] StatementId parse_expression_statement();
  void synchronize_statement();

  [[nodiscard]] ExpressionId parse_expression();
  [[nodiscard]] SourceRange expression_range(ExpressionId id) const;

  std::span<const Token> tokens_;
  const FileClassSymbols& symbols_;
  std::span<const ImportDecl> imports_;
  std::span<const MemberOutline> outlines_;
  DiagnosticEngine& diagnostics_;
  FileClassDecl file_class_;
  std::size_t current_{0};
  std::size_t limit_{0};
};

}  // namespace cloth

#endif  // CLOTH_PARSER_DEFINITION_PASS_H_
