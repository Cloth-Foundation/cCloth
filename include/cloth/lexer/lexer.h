#ifndef CLOTH_LEXER_LEXER_H_
#define CLOTH_LEXER_LEXER_H_

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/token.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cloth {

class Lexer {
 public:
  Lexer(const SourceFile& source, DiagnosticEngine& diagnostics) noexcept;

  [[nodiscard]] std::vector<Token> lex();

 private:
  [[nodiscard]] bool at_end() const noexcept;
  [[nodiscard]] char peek(std::size_t lookahead = 0) const noexcept;
  char advance() noexcept;
  bool match(char expected) noexcept;

  [[nodiscard]] SourceLocation current_location() const noexcept;
  [[nodiscard]] Token make_token(TokenKind kind, std::size_t start,
                                 SourceLocation location) const noexcept;

  void skip_ignored();
  [[nodiscard]] Token scan_identifier(std::size_t start,
                                      SourceLocation location);
  [[nodiscard]] Token scan_number(std::size_t start, SourceLocation location);
  [[nodiscard]] Token scan_string(std::size_t start, SourceLocation location);
  [[nodiscard]] Token scan_character(std::size_t start,
                                     SourceLocation location);
  void report_invalid_escape(SourceLocation location, char escaped);

  const SourceFile& source_;
  DiagnosticEngine& diagnostics_;
  std::string_view input_;
  std::size_t current_{0};
  std::uint32_t line_{1};
  std::uint32_t column_{1};
  bool previous_was_carriage_return_{false};
};

}  // namespace cloth

#endif  // CLOTH_LEXER_LEXER_H_
