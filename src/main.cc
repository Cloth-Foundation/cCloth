#include "cloth/ast/ast_printer.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir_printer.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir_printer.h"
#include "cloth/source/source_file.h"

#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

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
    const auto& location = diagnostic.range.begin;
    const auto file =
        location.file.empty() ? std::string_view{"<unknown>"} : location.file;
    std::cerr << file << ':' << location.line << ':' << location.column << ": "
              << cloth::diagnostic_severity_name(diagnostic.severity) << ": "
              << diagnostic.message << '\n';
  }
}

void print_tokens(std::span<const cloth::Token> tokens) {
  std::cout << "\nTokens:\n";

  for (const auto& token : tokens) {
    const auto& location = token.range.begin;
    std::ostringstream position;
    position << location.line << ':' << location.column;
    std::cout << std::left << std::setw(8) << position.str() << std::setw(24)
              << cloth::token_kind_name(token.kind)
              << escaped_lexeme(token.lexeme) << '\n';
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: clothc <source.co>...\n";
    return 2;
  }

  cloth::Compilation compilation;
  for (int index = 1; index < argc; ++index) {
    auto source_result =
        cloth::SourceFile::load(std::filesystem::path{argv[index]});
    if (!source_result) {
      const auto& error = source_result.error();
      std::cerr << error.path.generic_string() << ": error: " << error.message
                << '\n';
      return 2;
    }
    compilation.add_source(std::move(*source_result));
  }

  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult compilation_result =
      compilation.analyze(diagnostics);

  print_diagnostics(diagnostics);
  for (std::size_t index = 0; index < compilation.source_count(); ++index) {
    if (compilation.source_count() > 1) {
      std::cout << "\nSource " << compilation.source(index).display_path()
                << ":\n";
    }
    print_tokens(compilation.tokens(index));
    std::cout << "\nAST:\n";
    cloth::print_ast_summary(compilation.syntax(index), std::cout);
  }
  std::cout << "\nTyped HIR:\n";
  cloth::print_hir_summary(compilation_result.hir, compilation_result.semantics,
                           std::cout);
  std::cout << "\nControl-flow MIR:\n";
  cloth::print_mir_summary(compilation_result.mir, compilation_result.semantics,
                           std::cout);
  return diagnostics.has_errors() ? 1 : 0;
}
