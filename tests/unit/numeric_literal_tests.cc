// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/lexer/lexer.h"
#include "cloth/lexer/literal.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

class FrontendCompilation {
 public:
  explicit FrontendCompilation(std::string text) {
    compilation_.add_source(
        cloth::SourceFile::from_memory("Literals.co", std::move(text)));
    result.emplace(compilation_.analyze_frontend(diagnostics));
  }

  [[nodiscard]] bool has_diagnostic(std::string_view text) const {
    for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
      if (diagnostic.message.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] const cloth::FileClassDecl& syntax() const {
    return compilation_.syntax(0);
  }

  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::FrontendResult> result;

 private:
  cloth::Compilation compilation_;
};

struct ExpectedStaticConstant {
  std::string name;
  std::string type;
  std::uint64_t bits;
};

void add_static_constant(std::string& source,
                         std::vector<ExpectedStaticConstant>& expected,
                         std::string_view type, std::string_view name,
                         std::string_view expression, std::uint64_t bits) {
  source += "static final ";
  source += type;
  source += ' ';
  source += name;
  source += " = ";
  source += expression;
  source += ";\n";
  expected.push_back(
      ExpectedStaticConstant{std::string{name}, std::string{type}, bits});
}

void expect_static_constants(
    TestContext& test, const FrontendCompilation& compilation,
    const std::vector<ExpectedStaticConstant>& expected) {
  test.expect(compilation.result.has_value() && compilation.result->is_valid &&
                  !compilation.diagnostics.has_errors(),
              "static constant boundary fixture failed analysis");
  if (!compilation.result || !compilation.result->is_valid) {
    return;
  }
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  for (const ExpectedStaticConstant& value : expected) {
    const cloth::SemanticSymbol* found = nullptr;
    for (const cloth::SymbolId field : file.fields) {
      const cloth::SemanticSymbol& symbol = semantics.symbol(field);
      if (symbol.name == value.name) {
        found = &symbol;
        break;
      }
    }
    test.expect(found != nullptr, "static boundary field was not declared");
    if (found == nullptr) {
      continue;
    }
    test.expect(semantics.type(found->type).name == value.type,
                "static boundary field has the wrong type");
    test.expect(found->static_constant.has_value() &&
                    found->static_constant->bits == value.bits,
                "static boundary field has the wrong canonical bits");
  }
}

void canonical_suffixes_are_single_tokens(TestContext& test) {
  constexpr std::array<std::pair<std::string_view, cloth::TokenKind>, 13>
      kExpected{{{"1i8", cloth::TokenKind::kIntegerLiteral},
                 {"2i16", cloth::TokenKind::kIntegerLiteral},
                 {"3i32", cloth::TokenKind::kIntegerLiteral},
                 {"4i64", cloth::TokenKind::kIntegerLiteral},
                 {"5u8", cloth::TokenKind::kIntegerLiteral},
                 {"6u16", cloth::TokenKind::kIntegerLiteral},
                 {"7u32", cloth::TokenKind::kIntegerLiteral},
                 {"8u64", cloth::TokenKind::kIntegerLiteral},
                 {"9f32", cloth::TokenKind::kFloatLiteral},
                 {"10f64", cloth::TokenKind::kFloatLiteral},
                 {"11.0f32", cloth::TokenKind::kFloatLiteral},
                 {"12.0f64", cloth::TokenKind::kFloatLiteral},
                 {"i8", cloth::TokenKind::kIdentifier}}};
  const cloth::SourceFile source = cloth::SourceFile::from_memory(
      "tokens.co",
      "1i8 2i16 3i32 4i64 5u8 6u16 7u32 8u64 9f32 10f64 11.0f32 "
      "12.0f64 i8");
  cloth::DiagnosticEngine diagnostics;
  const std::vector<cloth::Token> tokens =
      cloth::Lexer{source, diagnostics}.lex();

  test.expect(!diagnostics.has_errors(),
              "canonical numeric suffixes produced lexer errors");
  test.expect(tokens.size() == kExpected.size() + 1,
              "canonical suffix token count changed");
  for (std::size_t index = 0; index < kExpected.size() && index < tokens.size();
       ++index) {
    test.expect(tokens[index].lexeme == kExpected[index].first,
                "numeric suffix was split from its literal");
    test.expect(tokens[index].kind == kExpected[index].second,
                "numeric suffix selected the wrong token kind");
  }
}

void malformed_suffixes_are_atomic(TestContext& test) {
  struct MalformedCase {
    std::string_view spelling;
    std::string_view message;
  };
  constexpr std::array<MalformedCase, 16> kCases{{
      {"1i32value", "invalid numeric suffix 'i32value'"},
      {"1I32", "invalid numeric suffix 'I32'"},
      {"1U8", "invalid numeric suffix 'U8'"},
      {"1F32", "invalid numeric suffix 'F32'"},
      {"1i7", "invalid numeric suffix 'i7'"},
      {"1u128", "invalid numeric suffix 'u128'"},
      {"1f16", "invalid numeric suffix 'f16'"},
      {"1i8u8", "invalid numeric suffix 'i8u8'"},
      {"1f32f64", "invalid numeric suffix 'f32f64'"},
      {"1byte", "invalid numeric suffix 'byte'"},
      {"1int", "invalid numeric suffix 'int'"},
      {"1uint", "invalid numeric suffix 'uint'"},
      {"1float", "invalid numeric suffix 'float'"},
      {"1f64name", "invalid numeric suffix 'f64name'"},
      {"1.0i32", "integer suffix 'i32' cannot be applied"},
      {"1_000", "invalid numeric suffix '_000'"},
  }};
  std::string text;
  for (const MalformedCase& malformed : kCases) {
    if (!text.empty()) {
      text += ' ';
    }
    text += malformed.spelling;
  }
  const cloth::SourceFile source =
      cloth::SourceFile::from_memory("bad.co", std::move(text));
  cloth::DiagnosticEngine diagnostics;
  const std::vector<cloth::Token> tokens =
      cloth::Lexer{source, diagnostics}.lex();

  test.expect(tokens.size() == kCases.size() + 1,
              "a malformed numeric tail was split into another token");
  for (std::size_t index = 0; index < kCases.size() && index < tokens.size();
       ++index) {
    test.expect(tokens[index].lexeme == kCases[index].spelling,
                "malformed numeric spelling was not consumed atomically");
  }
  test.expect(diagnostics.diagnostics().size() == kCases.size(),
              "each malformed numeric suffix should have one diagnostic");
  std::size_t previous_offset = 0;
  for (std::size_t index = 0;
       index < kCases.size() && index < diagnostics.diagnostics().size();
       ++index) {
    const cloth::Diagnostic& diagnostic = diagnostics.diagnostics()[index];
    test.expect(
        diagnostic.message.find(kCases[index].message) != std::string::npos,
        "malformed suffix diagnostic changed category or spelling");
    test.expect(
        index == 0 || diagnostic.range.begin.byte_offset > previous_offset,
        "malformed suffix diagnostics are not source ordered");
    previous_offset = diagnostic.range.begin.byte_offset;
  }
}

void integer_suffix_boundaries(TestContext& test) {
  struct SignedBoundary {
    std::string_view prefix;
    std::string_view type;
    std::string_view suffix;
    std::string_view minimum;
    std::string_view next_minimum;
    std::string_view previous_maximum;
    std::string_view maximum;
    std::uint64_t minimum_bits;
    std::uint64_t next_minimum_bits;
  };
  constexpr std::array<SignedBoundary, 4> kSigned{{
      {"I8", "int8", "i8", "128", "127", "126", "127", UINT64_C(128),
       UINT64_C(129)},
      {"I16", "int16", "i16", "32768", "32767", "32766", "32767",
       UINT64_C(32768), UINT64_C(32769)},
      {"I32", "int32", "i32", "2147483648", "2147483647", "2147483646",
       "2147483647", UINT64_C(2147483648), UINT64_C(2147483649)},
      {"I64", "int64", "i64", "9223372036854775808", "9223372036854775807",
       "9223372036854775806", "9223372036854775807",
       UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001)},
  }};
  struct UnsignedBoundary {
    std::string_view prefix;
    std::string_view type;
    std::string_view suffix;
    std::string_view previous_maximum;
    std::string_view maximum;
    std::uint64_t previous_maximum_bits;
    std::uint64_t maximum_bits;
  };
  constexpr std::array<UnsignedBoundary, 4> kUnsigned{{
      {"U8", "uint8", "u8", "254", "255", UINT64_C(254), UINT64_C(255)},
      {"U16", "uint16", "u16", "65534", "65535", UINT64_C(65534),
       UINT64_C(65535)},
      {"U32", "uint32", "u32", "4294967294", "4294967295", UINT64_C(4294967294),
       UINT64_C(4294967295)},
      {"U64", "uint64", "u64", "18446744073709551614", "18446744073709551615",
       UINT64_C(0xfffffffffffffffe), UINT64_C(0xffffffffffffffff)},
  }};

  std::string source;
  std::vector<ExpectedStaticConstant> expected;
  for (const SignedBoundary& boundary : kSigned) {
    add_static_constant(source, expected, boundary.type,
                        std::string{boundary.prefix} + "Zero",
                        std::string{"0"} + std::string{boundary.suffix}, 0);
    add_static_constant(source, expected, boundary.type,
                        std::string{boundary.prefix} + "Minimum",
                        std::string{"-"} + std::string{boundary.minimum} +
                            std::string{boundary.suffix},
                        boundary.minimum_bits);
    add_static_constant(source, expected, boundary.type,
                        std::string{boundary.prefix} + "NextMinimum",
                        std::string{"-"} + std::string{boundary.next_minimum} +
                            std::string{boundary.suffix},
                        boundary.next_minimum_bits);
    add_static_constant(
        source, expected, boundary.type,
        std::string{boundary.prefix} + "PreviousMaximum",
        std::string{boundary.previous_maximum} + std::string{boundary.suffix},
        std::stoull(std::string{boundary.previous_maximum}));
    add_static_constant(
        source, expected, boundary.type,
        std::string{boundary.prefix} + "Maximum",
        std::string{boundary.maximum} + std::string{boundary.suffix},
        std::stoull(std::string{boundary.maximum}));
  }
  for (const UnsignedBoundary& boundary : kUnsigned) {
    add_static_constant(source, expected, boundary.type,
                        std::string{boundary.prefix} + "Minimum",
                        std::string{"0"} + std::string{boundary.suffix}, 0);
    add_static_constant(source, expected, boundary.type,
                        std::string{boundary.prefix} + "NextMinimum",
                        std::string{"1"} + std::string{boundary.suffix}, 1);
    add_static_constant(
        source, expected, boundary.type,
        std::string{boundary.prefix} + "PreviousMaximum",
        std::string{boundary.previous_maximum} + std::string{boundary.suffix},
        boundary.previous_maximum_bits);
    add_static_constant(
        source, expected, boundary.type,
        std::string{boundary.prefix} + "Maximum",
        std::string{boundary.maximum} + std::string{boundary.suffix},
        boundary.maximum_bits);
  }
  expect_static_constants(test, FrontendCompilation{std::move(source)},
                          expected);

  constexpr std::array<std::pair<std::string_view, std::string_view>, 16>
      kOutOfRange{{
          {"int8", "-129i8"},
          {"int8", "128i8"},
          {"int16", "-32769i16"},
          {"int16", "32768i16"},
          {"int32", "-2147483649i32"},
          {"int32", "2147483648i32"},
          {"int64", "-9223372036854775809i64"},
          {"int64", "9223372036854775808i64"},
          {"uint8", "-1u8"},
          {"uint8", "256u8"},
          {"uint16", "-1u16"},
          {"uint16", "65536u16"},
          {"uint32", "-1u32"},
          {"uint32", "4294967296u32"},
          {"uint64", "-1u64"},
          {"uint64", "18446744073709551616u64"},
      }};
  std::string invalid_source;
  for (std::size_t index = 0; index < kOutOfRange.size(); ++index) {
    invalid_source += "static final ";
    invalid_source += kOutOfRange[index].first;
    invalid_source += " Bad" + std::to_string(index) + " = ";
    invalid_source += kOutOfRange[index].second;
    invalid_source += ";\n";
  }
  FrontendCompilation invalid{std::move(invalid_source)};
  test.expect(!invalid.result->is_valid,
              "out-of-range integer suffix boundaries were accepted");
  for (const auto& [unused, spelling] : kOutOfRange) {
    test.expect(
        invalid.has_diagnostic("integer literal '" + std::string{spelling} +
                               "' is out of range"),
        "integer suffix boundary produced no precise diagnostic");
  }
}

void floating_suffix_boundaries(TestContext& test) {
  const std::string f32_subnormal =
      "0." + std::string(44, '0') + "140129846f32";
  const std::string f64_subnormal =
      "0." + std::string(323, '0') + "49406564584124654f64";
  const std::string f32_maximum =
      "340282346638528859811704183484516925440.0f32";
  const std::string f64_maximum =
      "1797693134862315708145274237317043567980705675258449965989174768031572"
      "6078002853876058955863276687817154045895351438246423432132688946418276"
      "8467546703537516986049910576551282076245490090389328944075868508455133"
      "9423045832369032229481658085593321233482747978262041447231687381771809"
      "19299881250404026184124858368.0f64";
  std::string source;
  std::vector<ExpectedStaticConstant> expected;
  add_static_constant(source, expected, "float32", "F32Zero", "0f32", 0);
  add_static_constant(source, expected, "float32", "F32NegativeZero", "-0.0f32",
                      UINT64_C(0x80000000));
  add_static_constant(source, expected, "float32", "F32Tie",
                      "1.000000059604644775390625f32", UINT64_C(0x3f800000));
  add_static_constant(source, expected, "float32", "F32AboveTie",
                      "1.0000000596046447753906251f32", UINT64_C(0x3f800001));
  add_static_constant(source, expected, "float32", "F32Subnormal",
                      f32_subnormal, 1);
  add_static_constant(source, expected, "float32", "F32Maximum", f32_maximum,
                      UINT64_C(0x7f7fffff));
  add_static_constant(source, expected, "float32", "F32Minimum",
                      "-" + f32_maximum, UINT64_C(0xff7fffff));

  add_static_constant(source, expected, "float64", "F64Zero", "0f64", 0);
  add_static_constant(source, expected, "float64", "F64NegativeZero", "-0.0f64",
                      UINT64_C(0x8000000000000000));
  add_static_constant(
      source, expected, "float64", "F64Tie",
      "1.00000000000000011102230246251565404236316680908203125f64",
      UINT64_C(0x3ff0000000000000));
  add_static_constant(
      source, expected, "float64", "F64AboveTie",
      "1.000000000000000111022302462515654042363166809082031251f64",
      UINT64_C(0x3ff0000000000001));
  add_static_constant(source, expected, "float64", "F64Subnormal",
                      f64_subnormal, 1);
  add_static_constant(source, expected, "float64", "F64Maximum", f64_maximum,
                      UINT64_C(0x7fefffffffffffff));
  add_static_constant(source, expected, "float64", "F64Minimum",
                      "-" + f64_maximum, UINT64_C(0xffefffffffffffff));
  expect_static_constants(test, FrontendCompilation{std::move(source)},
                          expected);

  const std::array<std::pair<std::string, std::string_view>, 4> kInvalid{{
      {"0." + std::string(45, '0') + "1f32", "float32"},
      {"4" + std::string(38, '0') + ".0f32", "float32"},
      {"0." + std::string(324, '0') + "1f64", "float64"},
      {"2" + std::string(308, '0') + ".0f64", "float64"},
  }};
  std::string invalid_source;
  for (std::size_t index = 0; index < kInvalid.size(); ++index) {
    invalid_source += "static final ";
    invalid_source += kInvalid[index].second;
    invalid_source += " Bad" + std::to_string(index) + " = ";
    invalid_source += kInvalid[index].first;
    invalid_source += ";\n";
  }
  FrontendCompilation invalid{std::move(invalid_source)};
  test.expect(!invalid.result->is_valid,
              "floating suffix underflow or overflow was accepted");
  for (const auto& [spelling, type] : kInvalid) {
    test.expect(invalid.has_diagnostic("floating literal '" + spelling +
                                       "' is out of range for '" +
                                       std::string{type} + "'"),
                "floating suffix boundary produced no precise diagnostic");
  }
}

void unsuffixed_contexts_are_unchanged(TestContext& test) {
  FrontendCompilation compilation{R"(
    static final int8 I8 = 11;
    static final int16 I16 = 12;
    static final int32 I32 = 13;
    static final int64 I64 = 14;
    static final byte Byte = 15;
    static final uint8 U8 = 16;
    static final uint16 U16 = 17;
    static final uint32 U32 = 18;
    static final uint64 U64 = 19;
    static final float32 F32 = 20.0;
    static final float64 F64 = 21.0;
    static func IntegerDefault(): int32 { var value = 22; return value; }
    static func FloatDefault(): float64 { var value = 23.0; return value; }
  )"};
  test.expect(compilation.result->is_valid,
              "typed suffixes changed unsuffixed contextual behavior");
  const std::map<std::string, std::string_view> expected_types{
      {"11", "int8"},      {"12", "int16"},     {"13", "int32"},
      {"14", "int64"},     {"15", "byte"},      {"16", "uint8"},
      {"17", "uint16"},    {"18", "uint32"},    {"19", "uint64"},
      {"20.0", "float32"}, {"21.0", "float64"}, {"22", "int32"},
      {"23.0", "float64"},
  };
  const auto expressions = compilation.syntax().storage.expressions();
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  std::size_t checked = 0;
  for (std::size_t index = 0; index < expressions.size(); ++index) {
    const auto* literal =
        std::get_if<cloth::LiteralExpression>(&expressions[index].data);
    if (literal == nullptr || (literal->kind != cloth::LiteralKind::kInteger &&
                               literal->kind != cloth::LiteralKind::kFloat)) {
      continue;
    }
    const auto expected = expected_types.find(std::string{literal->lexeme});
    if (expected == expected_types.end()) {
      continue;
    }
    test.expect(
        semantics.type(file.expressions[index].type).name == expected->second,
        "an unsuffixed literal selected a different contextual type");
    ++checked;
  }
  test.expect(checked == expected_types.size(),
              "unsuffixed context matrix was not fully inspected");
}

void suffixes_count_toward_literal_limit(TestContext& test) {
  const std::string accepted = std::string(4094, '0') + "i8";
  FrontendCompilation valid{"static final int8 Value = " + accepted + ";"};
  test.expect(valid.result->is_valid,
              "a 4096-byte typed numeric token was rejected");

  const std::string rejected = std::string(4095, '0') + "i8";
  FrontendCompilation invalid{"static final int8 Value = " + rejected + ";"};
  test.expect(
      !invalid.result->is_valid &&
          invalid.has_diagnostic("constant numeric literal exceeds 4096 bytes"),
      "typed numeric token limit omitted its suffix bytes");
}

void suffixes_select_exact_types_and_lower_canonically(TestContext& test) {
  FrontendCompilation compilation{R"(
    static final int8 I8 = 1i8;
    static final int16 I16 = 2i16;
    static final int32 I32 = 3i32;
    static final int64 I64 = 4i64;
    static final uint8 U8 = 5u8;
    static final uint16 U16 = 6u16;
    static final uint32 U32 = 7u32;
    static final uint64 U64 = 8u64;
    static final float32 F32 = 9f32;
    static final float64 F64 = 10.0f64;
    static final int32 DefaultInteger = 11;
    static final float64 DefaultFloat = 12.0;
    static final int64 Widened = 13i8;
    static final int8 Minimum = -128i8;
    static final int8 Converted = int8(127i16);
  )"};

  test.expect(
      !compilation.diagnostics.has_errors() && compilation.result->is_valid,
      "valid typed numeric literals failed frontend analysis");
  const std::map<std::string, std::string_view> expected_types{
      {"1i8", "int8"},        {"2i16", "int16"},  {"3i32", "int32"},
      {"4i64", "int64"},      {"5u8", "uint8"},   {"6u16", "uint16"},
      {"7u32", "uint32"},     {"8u64", "uint64"}, {"9f32", "float32"},
      {"10.0f64", "float64"}, {"11", "int32"},    {"12.0", "float64"},
      {"13i8", "int8"},       {"128i8", "int8"},  {"127i16", "int16"}};
  const auto expressions = compilation.syntax().storage.expressions();
  const cloth::SemanticModel& semantics = compilation.result->semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  std::size_t checked = 0;
  bool retained_integer_core_float_kind = false;
  for (std::size_t index = 0; index < expressions.size(); ++index) {
    const auto* literal =
        std::get_if<cloth::LiteralExpression>(&expressions[index].data);
    if (literal == nullptr || (literal->kind != cloth::LiteralKind::kInteger &&
                               literal->kind != cloth::LiteralKind::kFloat)) {
      continue;
    }
    const auto expected = expected_types.find(std::string{literal->lexeme});
    test.expect(expected != expected_types.end(),
                "test source contains an untracked numeric literal");
    if (expected == expected_types.end()) {
      continue;
    }
    const std::string& actual =
        semantics.type(file.expressions[index].type).name;
    test.expect(actual == expected->second,
                "numeric suffix did not select its exact semantic type");
    retained_integer_core_float_kind =
        retained_integer_core_float_kind ||
        (literal->lexeme == "9f32" &&
         literal->kind == cloth::LiteralKind::kFloat);
    ++checked;
  }
  test.expect(checked == expected_types.size(),
              "not every numeric literal type was inspected");
  test.expect(retained_integer_core_float_kind,
              "AST did not classify an integer-core f32 token as floating");

  std::size_t lowered_literals = 0;
  bool found_integer_core_float = false;
  for (const cloth::HirExpression& expression :
       compilation.result->hir.storage.expressions()) {
    const auto* literal =
        std::get_if<cloth::HirLiteralExpression>(&expression.data);
    if (literal == nullptr || (literal->kind != cloth::LiteralKind::kInteger &&
                               literal->kind != cloth::LiteralKind::kFloat)) {
      continue;
    }
    const cloth::NumericLiteralSpelling spelling =
        cloth::parse_numeric_literal_spelling(literal->lexeme);
    test.expect(spelling.error == cloth::NumericLiteralSpellingError::kNone &&
                    spelling.suffix_kind == cloth::NumericLiteralSuffix::kNone,
                "HIR retained a source-only numeric suffix");
    found_integer_core_float =
        found_integer_core_float ||
        (literal->kind == cloth::LiteralKind::kFloat &&
         literal->lexeme == "9" &&
         semantics.type(expression.type).name == "float32");
    ++lowered_literals;
  }
  test.expect(lowered_literals == expected_types.size(),
              "typed numeric literals were lost during HIR lowering");
  test.expect(found_integer_core_float,
              "integer-core f32 literal was not retained as a float literal");

  for (const cloth::SymbolId field_id : file.fields) {
    const cloth::SemanticSymbol& field = semantics.symbol(field_id);
    test.expect(field.static_constant.has_value(),
                "typed numeric static field was not constant-evaluated");
  }
}

void exact_types_participate_in_overloads_and_widening(TestContext& test) {
  FrontendCompilation compilation{R"(
    func Pick(int8 value): int8 { return value; }
    func Pick(int64 value): int64 { return value; }
    func Wide(int64 value): int64 { return value; }
    func Run(): int64 {
      int8 exact = Pick(1i8);
      int64 widened = Wide(2i8);
      return exact + widened;
    }
    static func Cases(int64 value) {
      switch (value) { case 3i8: {} default: {} }
    }
  )"};

  test.expect(
      !compilation.diagnostics.has_errors() && compilation.result->is_valid,
      "exact typed literals did not select or widen correctly");
}

void invalid_typed_literals_are_rejected_without_cascades(TestContext& test) {
  FrontendCompilation compilation{R"(
    func Bad() {
      int8 narrow = 1i32;
      byte distinct = 2u8;
      var positive = 128i8;
      var negative = -1u8;
      int8 converted = int8(128i16);
    }
    static func Cases(int8 value) {
      switch (value) { case 3i16: {} default: {} }
    }
    func Malformed() { var value = 1i99; }
  )"};

  test.expect(!compilation.result->is_valid,
              "invalid typed numeric literals were accepted");
  test.expect(compilation.has_diagnostic(
                  "local initializer has type 'int32'; expected 'int8'") &&
                  compilation.has_diagnostic(
                      "local initializer has type 'uint8'; expected 'byte'") &&
                  compilation.has_diagnostic(
                      "integer literal '128i8' is out of range for 'int8'") &&
                  compilation.has_diagnostic(
                      "integer literal '-1u8' is out of range for 'uint8'") &&
                  compilation.has_diagnostic(
                      "out of range for explicit conversion to 'int8'") &&
                  compilation.has_diagnostic(
                      "case constant of type 'int16' cannot be used") &&
                  compilation.has_diagnostic("invalid numeric suffix 'i99'"),
              "invalid typed literals produced incomplete diagnostics");
  test.expect(!compilation.has_diagnostic("internal"),
              "malformed numeric recovery leaked an internal diagnostic");
}

void hir_verifier_rejects_malformed_numeric_literals(TestContext& test) {
  FrontendCompilation compilation{"func Value(): int32 { return 1; }"};
  test.expect(compilation.result->is_valid,
              "verification fixture failed frontend analysis");
  struct MalformedHir {
    cloth::LiteralKind kind;
    std::string_view type;
    std::string_view lexeme;
  };
  constexpr std::array<MalformedHir, 7> kMalformed{{
      {cloth::LiteralKind::kInteger, "int32", "1i32"},
      {cloth::LiteralKind::kFloat, "float32", "1f32"},
      {cloth::LiteralKind::kInteger, "int32", "1."},
      {cloth::LiteralKind::kInteger, "int32", "1.0"},
      {cloth::LiteralKind::kInteger, "bool", "1"},
      {cloth::LiteralKind::kInteger, "int8", "128"},
      {cloth::LiteralKind::kInteger, "int8", "129"},
  }};
  for (const MalformedHir& mutation : kMalformed) {
    cloth::HirModule malformed;
    static_cast<void>(malformed.storage.add_expression(cloth::HirExpression{
        *compilation.result->semantics.find_type(mutation.type),
        cloth::point_range(cloth::SourceLocation{"forged.co", 0, 1, 1}),
        cloth::HirLiteralExpression{mutation.kind,
                                    std::string{mutation.lexeme}}}));
    cloth::DiagnosticEngine diagnostics;
    test.expect(!cloth::verify_hir(malformed, compilation.result->semantics,
                                   diagnostics),
                "HIR verifier accepted a malformed numeric literal");
    bool found_invariant = false;
    for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
      found_invariant =
          found_invariant ||
          diagnostic.message.find("invalid canonical spelling or type") !=
              std::string::npos;
    }
    test.expect(found_invariant,
                "HIR numeric invariant produced the wrong diagnostic");
  }
}

}  // namespace

int main() {
  const std::array<TestCase, 10> tests{{
      {"canonical suffixes are single tokens",
       canonical_suffixes_are_single_tokens},
      {"malformed suffixes are atomic", malformed_suffixes_are_atomic},
      {"integer suffix boundaries", integer_suffix_boundaries},
      {"floating suffix boundaries", floating_suffix_boundaries},
      {"unsuffixed contexts are unchanged", unsuffixed_contexts_are_unchanged},
      {"suffixes count toward literal limit",
       suffixes_count_toward_literal_limit},
      {"suffixes select exact types and lower canonically",
       suffixes_select_exact_types_and_lower_canonically},
      {"exact types participate in overloads and widening",
       exact_types_participate_in_overloads_and_widening},
      {"invalid typed literals are rejected without cascades",
       invalid_typed_literals_are_rejected_without_cascades},
      {"HIR verifier rejects malformed numeric literals",
       hir_verifier_rejects_malformed_numeric_literals},
  }};
  return cloth::test::run_tests(tests);
}
