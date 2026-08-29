#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/parser/parser.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// libFuzzer requires this exact external entry-point name.
extern "C" int LLVMFuzzerTestOneInput(  // NOLINT(readability-identifier-naming)
    const std::uint8_t* data, std::size_t size) {
  constexpr std::size_t kMaximumInputSize = 1024U * 1024U;
  if (size > kMaximumInputSize) {
    return 0;
  }

  std::string text;
  if (size != 0) {
    text.assign(reinterpret_cast<const char*>(data), size);
  }
  cloth::SourceFile source =
      cloth::SourceFile::from_memory("Fuzz.co", std::move(text));
  cloth::DiagnosticEngine diagnostics;
  const std::vector<cloth::Token> tokens =
      cloth::Lexer{source, diagnostics}.lex();
  static_cast<void>(cloth::Parser{source, tokens, diagnostics}.parse());
  return 0;
}
