#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/lexer/token.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct LexedSource {
  cloth::SourceFile source;
  cloth::DiagnosticEngine diagnostics;
  std::vector<cloth::Token> tokens;

  explicit LexedSource(std::string text)
      : source(cloth::SourceFile::from_memory("test.co", std::move(text))) {
    tokens = cloth::Lexer{source, diagnostics}.lex();
  }
};

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

void expect_kinds(TestContext& test, const LexedSource& source,
                  std::initializer_list<cloth::TokenKind> expected) {
  test.expect(source.tokens.size() == expected.size(),
              "token count does not match");
  const auto count = source.tokens.size() < expected.size()
                         ? source.tokens.size()
                         : expected.size();
  auto iterator = expected.begin();
  for (std::size_t index = 0; index < count; ++index, ++iterator) {
    if (source.tokens[index].kind != *iterator) {
      std::cerr << "    token " << index << ": expected "
                << cloth::token_kind_name(*iterator) << ", got "
                << cloth::token_kind_name(source.tokens[index].kind) << '\n';
      test.expect(false, "token kind does not match");
    }
  }
}

void expect_token(TestContext& test, const LexedSource& source,
                  std::size_t index, cloth::TokenKind kind,
                  std::string_view lexeme, std::uint32_t line,
                  std::uint32_t column, std::size_t byte_offset) {
  if (index >= source.tokens.size()) {
    test.expect(false, "missing expected token");
    return;
  }

  const auto& token = source.tokens[index];
  test.expect(token.kind == kind, "unexpected token kind");
  test.expect(token.lexeme == lexeme, "unexpected token lexeme");
  test.expect(token.range.begin.file == "test.co", "unexpected source file");
  test.expect(token.range.begin.line == line, "unexpected token line");
  test.expect(token.range.begin.column == column, "unexpected token column");
  test.expect(token.range.begin.byte_offset == byte_offset,
              "unexpected token byte offset");
  test.expect(token.range.end.byte_offset == byte_offset + lexeme.size(),
              "unexpected token range end");
}

void empty_source(TestContext& test) {
  const LexedSource source{""};
  expect_kinds(test, source, {cloth::TokenKind::kEof});
  expect_token(test, source, 0, cloth::TokenKind::kEof, "", 1, 1, 0);
  test.expect(!source.diagnostics.has_errors(), "empty source should be valid");
}

void single_identifier(TestContext& test) {
  const LexedSource source{"cloth"};
  expect_kinds(test, source,
               {cloth::TokenKind::kIdentifier, cloth::TokenKind::kEof});
  expect_token(test, source, 0, cloth::TokenKind::kIdentifier, "cloth", 1, 1,
               0);
}

void keywords_versus_identifiers(TestContext& test) {
  const LexedSource source{
      "func function integer intValue functionName return"};
  expect_kinds(test, source,
               {cloth::TokenKind::kKwFunc, cloth::TokenKind::kIdentifier,
                cloth::TokenKind::kIdentifier, cloth::TokenKind::kIdentifier,
                cloth::TokenKind::kIdentifier, cloth::TokenKind::kKwReturn,
                cloth::TokenKind::kEof});
}

void primitive_keywords(TestContext& test) {
  const LexedSource source{
      "int int8 int16 int32 int64 uint uint8 uint16 uint32 uint64 "
      "float32 float64 bool char byte"};
  expect_kinds(test, source,
               {cloth::TokenKind::kKwInt, cloth::TokenKind::kKwInt8,
                cloth::TokenKind::kKwInt16, cloth::TokenKind::kKwInt32,
                cloth::TokenKind::kKwInt64, cloth::TokenKind::kKwUint,
                cloth::TokenKind::kKwUint8, cloth::TokenKind::kKwUint16,
                cloth::TokenKind::kKwUint32, cloth::TokenKind::kKwUint64,
                cloth::TokenKind::kKwFloat32, cloth::TokenKind::kKwFloat64,
                cloth::TokenKind::kKwBool, cloth::TokenKind::kKwChar,
                cloth::TokenKind::kKwByte, cloth::TokenKind::kEof});
}

void numeric_literals(TestContext& test) {
  const LexedSource source{"0 10 12345 3.14 0.5 42.0 42."};
  expect_kinds(
      test, source,
      {cloth::TokenKind::kIntegerLiteral, cloth::TokenKind::kIntegerLiteral,
       cloth::TokenKind::kIntegerLiteral, cloth::TokenKind::kFloatLiteral,
       cloth::TokenKind::kFloatLiteral, cloth::TokenKind::kFloatLiteral,
       cloth::TokenKind::kIntegerLiteral, cloth::TokenKind::kDot,
       cloth::TokenKind::kEof});
  expect_token(test, source, 3, cloth::TokenKind::kFloatLiteral, "3.14", 1, 12,
               11);
}

void punctuation(TestContext& test) {
  const LexedSource source{"(){}[],;:.?"};
  expect_kinds(test, source,
               {cloth::TokenKind::kLeftParen, cloth::TokenKind::kRightParen,
                cloth::TokenKind::kLeftBrace, cloth::TokenKind::kRightBrace,
                cloth::TokenKind::kLeftBracket, cloth::TokenKind::kRightBracket,
                cloth::TokenKind::kComma, cloth::TokenKind::kSemicolon,
                cloth::TokenKind::kColon, cloth::TokenKind::kDot,
                cloth::TokenKind::kQuestion, cloth::TokenKind::kEof});
}

void operators_and_longest_match(TestContext& test) {
  const LexedSource source{
      "+ - * / % = == != < <= > >= && || ! & | ^ ~ ++ -- += -= *= "
      "/= %= << >> <<= >>= &= |= ^="};
  expect_kinds(test, source,
               {cloth::TokenKind::kPlus,
                cloth::TokenKind::kMinus,
                cloth::TokenKind::kStar,
                cloth::TokenKind::kSlash,
                cloth::TokenKind::kPercent,
                cloth::TokenKind::kEqual,
                cloth::TokenKind::kEqualEqual,
                cloth::TokenKind::kBangEqual,
                cloth::TokenKind::kLess,
                cloth::TokenKind::kLessEqual,
                cloth::TokenKind::kGreater,
                cloth::TokenKind::kGreaterEqual,
                cloth::TokenKind::kAmpersandAmpersand,
                cloth::TokenKind::kPipePipe,
                cloth::TokenKind::kBang,
                cloth::TokenKind::kAmpersand,
                cloth::TokenKind::kPipe,
                cloth::TokenKind::kCaret,
                cloth::TokenKind::kTilde,
                cloth::TokenKind::kPlusPlus,
                cloth::TokenKind::kMinusMinus,
                cloth::TokenKind::kPlusEqual,
                cloth::TokenKind::kMinusEqual,
                cloth::TokenKind::kStarEqual,
                cloth::TokenKind::kSlashEqual,
                cloth::TokenKind::kPercentEqual,
                cloth::TokenKind::kShiftLeft,
                cloth::TokenKind::kShiftRight,
                cloth::TokenKind::kShiftLeftEqual,
                cloth::TokenKind::kShiftRightEqual,
                cloth::TokenKind::kAmpersandEqual,
                cloth::TokenKind::kPipeEqual,
                cloth::TokenKind::kCaretEqual,
                cloth::TokenKind::kEof});
}

void comments_are_skipped(TestContext& test) {
  const LexedSource source{
      "alpha // line comment\n beta /* block\ncomment */ gamma"};
  expect_kinds(test, source,
               {cloth::TokenKind::kIdentifier, cloth::TokenKind::kIdentifier,
                cloth::TokenKind::kIdentifier, cloth::TokenKind::kEof});
  expect_token(test, source, 1, cloth::TokenKind::kIdentifier, "beta", 2, 2,
               23);
  expect_token(test, source, 2, cloth::TokenKind::kIdentifier, "gamma", 3, 12,
               48);
  test.expect(!source.diagnostics.has_errors(), "comments should be valid");
}

void unterminated_block_comment(TestContext& test) {
  const LexedSource source{"x /* never closed"};
  expect_kinds(test, source,
               {cloth::TokenKind::kIdentifier, cloth::TokenKind::kEof});
  test.expect(source.diagnostics.diagnostics().size() == 1,
              "expected one diagnostic");
  if (!source.diagnostics.diagnostics().empty()) {
    const auto& diagnostic = source.diagnostics.diagnostics().front();
    test.expect(diagnostic.message == "unterminated block comment",
                "unexpected diagnostic message");
    test.expect(diagnostic.range.begin.column == 3,
                "block comment diagnostic should point at slash");
  }
}

void strings_and_escapes(TestContext& test) {
  const LexedSource source{
      R"cloth("hello" "say \"hi\"" "line\nnext" "nul\0")cloth"};
  expect_kinds(
      test, source,
      {cloth::TokenKind::kStringLiteral, cloth::TokenKind::kStringLiteral,
       cloth::TokenKind::kStringLiteral, cloth::TokenKind::kStringLiteral,
       cloth::TokenKind::kEof});
  test.expect(!source.diagnostics.has_errors(),
              "supported string escapes should be valid");
}

void unterminated_string(TestContext& test) {
  const LexedSource source{"\"not closed"};
  expect_kinds(test, source,
               {cloth::TokenKind::kStringLiteral, cloth::TokenKind::kEof});
  test.expect(source.diagnostics.has_errors(),
              "unterminated string should be rejected");
  test.expect(source.diagnostics.diagnostics().front().message ==
                  "unterminated string literal",
              "unexpected string diagnostic");
}

void character_literals(TestContext& test) {
  const LexedSource source{R"cloth('a' '\n' '\\')cloth"};
  expect_kinds(
      test, source,
      {cloth::TokenKind::kCharacterLiteral, cloth::TokenKind::kCharacterLiteral,
       cloth::TokenKind::kCharacterLiteral, cloth::TokenKind::kEof});
  test.expect(!source.diagnostics.has_errors(),
              "valid character literals should be accepted");
}

void malformed_character_literals(TestContext& test) {
  const LexedSource source{"'' 'ab' 'a"};
  expect_kinds(
      test, source,
      {cloth::TokenKind::kCharacterLiteral, cloth::TokenKind::kCharacterLiteral,
       cloth::TokenKind::kCharacterLiteral, cloth::TokenKind::kEof});
  test.expect(source.diagnostics.diagnostics().size() == 3,
              "each malformed character should produce one diagnostic");
  if (source.diagnostics.diagnostics().size() == 3) {
    test.expect(source.diagnostics.diagnostics()[2].message ==
                    "unterminated character literal",
                "missing unterminated character diagnostic");
  }
}

void invalid_characters_recover(TestContext& test) {
  const LexedSource source{"first @ second $ third"};
  expect_kinds(test, source,
               {cloth::TokenKind::kIdentifier, cloth::TokenKind::kIdentifier,
                cloth::TokenKind::kIdentifier, cloth::TokenKind::kEof});
  test.expect(source.diagnostics.diagnostics().size() == 2,
              "expected both invalid characters to be diagnosed");
  if (source.diagnostics.diagnostics().size() == 2) {
    test.expect(source.diagnostics.diagnostics()[0].range.begin.column == 7,
                "first invalid character location is wrong");
    test.expect(source.diagnostics.diagnostics()[1].range.begin.column == 16,
                "second invalid character location is wrong");
  }
}

void source_locations_and_eof(TestContext& test) {
  const LexedSource source{"a\r\n  b\n\tc"};
  expect_kinds(test, source,
               {cloth::TokenKind::kIdentifier, cloth::TokenKind::kIdentifier,
                cloth::TokenKind::kIdentifier, cloth::TokenKind::kEof});
  expect_token(test, source, 0, cloth::TokenKind::kIdentifier, "a", 1, 1, 0);
  expect_token(test, source, 1, cloth::TokenKind::kIdentifier, "b", 2, 3, 5);
  expect_token(test, source, 2, cloth::TokenKind::kIdentifier, "c", 3, 2, 8);
  expect_token(test, source, 3, cloth::TokenKind::kEof, "", 3, 3, 9);
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
      {"single identifier", single_identifier},
      {"keywords versus identifiers", keywords_versus_identifiers},
      {"primitive keywords", primitive_keywords},
      {"numeric literals", numeric_literals},
      {"punctuation", punctuation},
      {"operators and longest match", operators_and_longest_match},
      {"comments are skipped", comments_are_skipped},
      {"unterminated block comment", unterminated_block_comment},
      {"strings and escapes", strings_and_escapes},
      {"unterminated string", unterminated_string},
      {"character literals", character_literals},
      {"malformed character literals", malformed_character_literals},
      {"invalid characters recover", invalid_characters_recover},
      {"source locations and eof", source_locations_and_eof},
  };

  int failures = 0;
  for (const auto& test_case : tests) {
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
