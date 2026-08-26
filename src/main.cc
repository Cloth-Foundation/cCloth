#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/lexer/token.h"
#include "cloth/source/source_file.h"

#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string escaped_lexeme(std::string_view lexeme) {
  std::ostringstream output;
  output << '"';
  for (const char character : lexeme) {
    switch (character) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      case '\0':
        output << "\\0";
        break;
      default: {
        const auto value = static_cast<unsigned char>(character);
        if (std::isprint(value) != 0) {
          output << character;
        } else {
          output << "\\x" << std::uppercase << std::hex << std::setw(2)
                 << std::setfill('0') << static_cast<unsigned int>(value)
                 << std::dec << std::nouppercase << std::setfill(' ');
        }
        break;
      }
    }
  }
  output << '"';
  return output.str();
}

void print_diagnostics(const cloth::DiagnosticEngine& engine) {
  for (const auto& diagnostic : engine.diagnostics()) {
    const auto file = diagnostic.location.file.empty()
                          ? std::string_view{"<unknown>"}
                          : diagnostic.location.file;
    std::cerr << file << ':' << diagnostic.location.line << ':'
              << diagnostic.location.column << ": "
              << cloth::diagnostic_severity_name(diagnostic.severity) << ": "
              << diagnostic.message << '\n';
  }
}

void print_tokens(const std::vector<cloth::Token>& tokens) {
  std::cout << "\nTokens:\n";

  for (const auto& token : tokens) {
    std::ostringstream position;
    position << token.location.line << ':' << token.location.column;
    std::cout << std::left << std::setw(8) << position.str() << std::setw(24)
              << cloth::token_kind_name(token.kind)
              << escaped_lexeme(token.lexeme) << '\n';
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: clothc <source.co>\n";
    return 2;
  }

  auto source_result = cloth::SourceFile::load(std::filesystem::path{argv[1]});
  if (!source_result) {
    const auto& error = source_result.error();
    std::cerr << error.path.generic_string() << ": error: " << error.message
              << '\n';
    return 2;
  }

  auto source = std::move(*source_result);
  cloth::DiagnosticEngine diagnostics;
  const auto tokens = cloth::Lexer{source, diagnostics}.lex();

  print_diagnostics(diagnostics);
  print_tokens(tokens);
  return diagnostics.has_errors() ? 1 : 0;
}
