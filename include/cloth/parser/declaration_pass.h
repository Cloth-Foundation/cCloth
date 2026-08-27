#ifndef CLOTH_PARSER_DECLARATION_PASS_H_
#define CLOTH_PARSER_DECLARATION_PASS_H_

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/file_class_symbols.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cloth {

struct TokenIndexRange {
  std::size_t begin;
  std::size_t end;
};

struct MemberOutline {
  DeclarationKind kind;
  std::size_t symbol_index;
  std::size_t begin_token;
  std::optional<TokenIndexRange> initializer_tokens;
  std::optional<TokenIndexRange> body_tokens;
  SourceRange range;
  bool is_valid{true};
};

struct DeclarationPassResult {
  FileClassSymbols symbols;
  std::vector<ImportDecl> imports;
  std::vector<MemberOutline> outlines;
  bool is_valid{true};
};

class DeclarationPass {
 public:
  DeclarationPass(const SourceFile& source, std::span<const Token> tokens,
                  DiagnosticEngine& diagnostics);

  [[nodiscard]] DeclarationPassResult run();

 private:
  [[nodiscard]] bool at_end() const noexcept;
  [[nodiscard]] const Token& current() const noexcept;
  [[nodiscard]] const Token& peek(std::size_t lookahead) const noexcept;
  const Token& advance() noexcept;
  bool match(TokenKind kind) noexcept;

  [[nodiscard]] std::optional<TypeSyntax> parse_type();
  [[nodiscard]] std::vector<ParameterSymbol> parse_parameters(
      std::string_view declaration_name);
  [[nodiscard]] std::optional<TokenIndexRange> locate_body(
      std::string_view declaration_name);

  void parse_field();
  void parse_function();
  void parse_constructor();
  void parse_import();
  void skip_deferred_nested_type();
  void synchronize_member();

  [[nodiscard]] std::size_t add_symbol(MemberSymbol symbol);
  [[nodiscard]] bool has_duplicate(const MemberSymbol& symbol);
  [[nodiscard]] bool looks_like_member_start(std::size_t index) const noexcept;
  [[nodiscard]] SourceRange range_from(std::size_t begin,
                                       std::size_t end) const noexcept;

  const SourceFile& source_;
  std::span<const Token> tokens_;
  DiagnosticEngine& diagnostics_;
  FileClassSymbols symbols_;
  std::vector<ImportDecl> imports_;
  std::vector<MemberOutline> outlines_;
  std::size_t current_{0};
  bool is_valid_{true};
};

}  // namespace cloth

#endif  // CLOTH_PARSER_DECLARATION_PASS_H_
