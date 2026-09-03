// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/sema/scalar_constants.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test.h"

namespace {
using cloth::ConstantBits;
using cloth::ConstantError;
using cloth::LiteralKind;
using cloth::TokenKind;
using cloth::TypeKind;
using cloth::test::TestCase;
using cloth::test::TestContext;

void bits(TestContext& test, ConstantBits actual, std::uint64_t expected) {
  test.expect(
      actual && *actual == expected,
      "expected bits " + std::to_string(expected) + ", got " +
          (actual
               ? std::to_string(*actual)
               : std::string{cloth::constant_error_message(actual.error())}));
}

void error(TestContext& test, ConstantBits actual, ConstantError expected) {
  test.expect(
      !actual && actual.error() == expected,
      "expected " + std::string{cloth::constant_error_message(expected)});
}

void integer_values(TestContext& test) {
  using enum TypeKind;
  using enum TokenKind;
  bits(test,
       cloth::scalar_literal(LiteralKind::kInteger, "18446744073709551615",
                             kUint64),
       UINT64_MAX);
  bits(test,
       cloth::scalar_literal(LiteralKind::kInteger, "9223372036854775808",
                             kInt64, true),
       UINT64_C(0x8000000000000000));
  error(test,
        cloth::scalar_literal(LiteralKind::kInteger, "18446744073709551616",
                              kUint64),
        ConstantError::kOutOfRange);
  error(test, cloth::scalar_literal(LiteralKind::kInteger, "-", kInt32),
        ConstantError::kInvalidLiteral);
  for (const auto type : {kInt8, kByte, kUint8}) {
    const bool signed_type = type == kInt8;
    for (int a = 0; a < 256; ++a) {
      for (int b = 0; b < 256; ++b) {
        const int left = signed_type && a >= 128 ? a - 256 : a;
        const int right = signed_type && b >= 128 ? b - 256 : b;
        for (auto op : {kPlus, kMinus, kStar, kSlash, kPercent}) {
          const auto actual =
              cloth::binary_scalar(op, type, static_cast<std::uint64_t>(a),
                                   static_cast<std::uint64_t>(b), type);
          if ((op == kSlash || op == kPercent) && right == 0) {
            error(test, actual, ConstantError::kZeroDivisor);
            continue;
          }
          if ((op == kSlash || op == kPercent) && left == -128 && right == -1) {
            error(test, actual, ConstantError::kOverflow);
            continue;
          }
          const int value = op == kPlus    ? left + right
                            : op == kMinus ? left - right
                            : op == kStar  ? left * right
                            : op == kSlash ? left / right
                                           : left % right;
          if (value < (signed_type ? -128 : 0) ||
              value > (signed_type ? 127 : 255))
            error(test, actual, ConstantError::kOverflow);
          else
            bits(test, actual, static_cast<std::uint64_t>(value) & 255);
        }
      }
    }
  }
  bits(test, cloth::binary_scalar(kShiftRight, kInt8, 128, 7, kInt32), 255);
  bits(test, cloth::binary_scalar(kShiftLeft, kInt8, 127, 1, kInt32), 254);
  bits(test, cloth::binary_scalar(kShiftLeft, kUint64, UINT64_MAX, 63, kInt32),
       UINT64_C(0x8000000000000000));
  error(test, cloth::binary_scalar(kShiftLeft, kUint8, 1, 8, kInt32),
        ConstantError::kInvalidShift);
  error(test, cloth::binary_scalar(kShiftRight, kInt32, 1, UINT32_MAX, kInt32),
        ConstantError::kInvalidShift);
  error(test, cloth::binary_scalar(kPlus, kUint64, UINT64_MAX, 1, kUint64),
        ConstantError::kOverflow);
  error(test, cloth::unary_scalar(kMinus, kInt64, UINT64_C(0x8000000000000000)),
        ConstantError::kOverflow);
  bits(test, cloth::unary_scalar(kTilde, kUint16, 1), 65534);
}

void floating_vectors(TestContext& test) {
  using enum TypeKind;
  using enum TokenKind;
  struct Vector {
    TypeKind type;
    TokenKind op;
    std::uint64_t left;
    std::uint64_t right;
    std::uint64_t result;
  };
  // Exact IEEE encodings: ties-to-even, cancellation, gradual underflow,
  // signed zeros, the subnormal/normal boundary, and recurring quotients.
  constexpr Vector vectors[]{
      {kFloat32, kSlash, 0x3f800000, 0x40400000, 0x3eaaaaab},
      {kFloat32, kPlus, 0x3f800000, 0x33800000, 0x3f800000},
      {kFloat32, kPlus, 0x3f800001, 0x33800000, 0x3f800002},
      {kFloat32, kMinus, 0x3f800001, 0x3f800000, 0x34000000},
      {kFloat32, kPlus, 0x007fffff, 1, 0x00800000},
      {kFloat32, kMinus, 0x00800000, 1, 0x007fffff},
      {kFloat32, kSlash, 1, 0x40000000, 0},
      {kFloat32, kSlash, 3, 0x40000000, 2},
      {kFloat32, kSlash, 0x80000001, 0x40000000, 0x80000000},
      {kFloat32, kPlus, 0x80000000, 0x80000000, 0x80000000},
      {kFloat32, kMinus, 0x80000000, 0, 0x80000000},
      {kFloat32, kPlus, 0x3f800000, 0xbf800000, 0},
      {kFloat32, kStar, 0x80000000, 0xbf800000, 0},
      {kFloat32, kEqualEqual, 0, 0x80000000, 1},
      {kFloat32, kLess, 0xbf800000, 0, 1},
      {kFloat64, kSlash, 0x3ff0000000000000, 0x4008000000000000,
       0x3fd5555555555555},
      {kFloat64, kPlus, 0x3ff0000000000000, 0x3ca0000000000000,
       0x3ff0000000000000},
      {kFloat64, kPlus, 0x3ff0000000000001, 0x3ca0000000000000,
       0x3ff0000000000002},
      {kFloat64, kMinus, 0x3ff0000000000001, 0x3ff0000000000000,
       0x3cb0000000000000},
      {kFloat64, kPlus, 0x000fffffffffffff, 1, 0x0010000000000000},
      {kFloat64, kSlash, 1, 0x4000000000000000, 0},
      {kFloat64, kSlash, 3, 0x4000000000000000, 2},
      {kFloat64, kStar, 0x8000000000000001, 0x3fe0000000000000,
       0x8000000000000000},
      {kFloat64, kPlus, 0x8000000000000000, 0x8000000000000000,
       0x8000000000000000},
      {kFloat64, kLess, 0xbff0000000000000, 0xbfe0000000000000, 1},
  };
  const int saved = std::fegetround();
  for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    if (std::fesetround(mode) != 0) continue;
    for (const auto& v : vectors)
      bits(test, cloth::binary_scalar(v.op, v.type, v.left, v.right, v.type),
           v.result);
    bits(test,
         cloth::scalar_literal(LiteralKind::kFloat,
                               "1.000000059604644775390625", kFloat32),
         0x3f800000);
    bits(test,
         cloth::scalar_literal(LiteralKind::kFloat,
                               "1.0000000596046447753906251", kFloat32),
         0x3f800001);
    bits(test,
         cloth::scalar_literal(
             LiteralKind::kFloat,
             "1.00000000000000011102230246251565404236316680908203125",
             kFloat64),
         0x3ff0000000000000);
    bits(test,
         cloth::scalar_literal(
             LiteralKind::kFloat,
             "1.000000000000000111022302462515654042363166809082031251",
             kFloat64),
         0x3ff0000000000001);
    bits(test, cloth::scalar_literal(LiteralKind::kFloat, "-0.0", kFloat64),
         0x8000000000000000);
    bits(test,
         cloth::scalar_literal(
             LiteralKind::kFloat,
             "0." + std::string(323, '0') + "49406564584124654", kFloat64),
         1);
    bits(test,
         cloth::scalar_literal(LiteralKind::kFloat,
                               "0." + std::string(44, '0') + "140129846",
                               kFloat32),
         1);
    error(test,
          cloth::scalar_literal(LiteralKind::kFloat,
                                "0." + std::string(45, '0') + "1", kFloat32),
          ConstantError::kOutOfRange);
  }
  static_cast<void>(std::fesetround(saved));
  error(test,
        cloth::binary_scalar(kStar, kFloat32, 0x7f7fffff, 0x40000000, kFloat32),
        ConstantError::kNonFinite);
  error(test,
        cloth::binary_scalar(kPlus, kFloat64, 0x7fefffffffffffff,
                             0x7fefffffffffffff, kFloat64),
        ConstantError::kNonFinite);
  error(test,
        cloth::binary_scalar(kSlash, kFloat64, 0, 0x8000000000000000, kFloat64),
        ConstantError::kZeroDivisor);
  test.expect(!cloth::is_valid_scalar_bits(kFloat32, 0x7f800000),
              "infinity rejected");
  test.expect(!cloth::is_valid_scalar_bits(kFloat64, 0x7ff8000000000000),
              "NaN rejected");
  test.expect(!cloth::is_valid_scalar_bits(kFloat32, UINT64_C(1) << 32),
              "high bits rejected");
}

void scalar_boundaries(TestContext& test) {
  using enum TypeKind;
  using enum TokenKind;
  for (const auto& [type, width] : {std::pair{kInt8, 8U},
                                    {kInt16, 16U},
                                    {kInt32, 32U},
                                    {kInt64, 64U},
                                    {kByte, 8U},
                                    {kUint8, 8U},
                                    {kUint16, 16U},
                                    {kUint32, 32U},
                                    {kUint64, 64U}}) {
    const bool is_signed =
        type == kInt8 || type == kInt16 || type == kInt32 || type == kInt64;
    const auto mask = UINT64_MAX >> (64 - width);
    const auto minimum = std::uint64_t{1} << (width - 1);
    const auto maximum = is_signed ? minimum - 1 : mask;
    error(test, cloth::binary_scalar(kPlus, type, maximum, 1, type),
          ConstantError::kOverflow);
    error(test, cloth::binary_scalar(kStar, type, maximum, 2, type),
          ConstantError::kOverflow);
    bits(test, cloth::binary_scalar(kAmpersand, type, mask, 1, type), 1);
    bits(test, cloth::binary_scalar(kPipe, type, maximum, 1, type), maximum);
    bits(test, cloth::binary_scalar(kCaret, type, mask, mask, type), 0);
    bits(test, cloth::unary_scalar(kTilde, type, 0), mask);
    bits(test, cloth::unary_scalar(kPlus, type, maximum), maximum);
    bits(test, cloth::binary_scalar(kShiftLeft, type, mask, 1, kInt32),
         mask - 1);
    error(test, cloth::binary_scalar(kShiftLeft, type, 1, width, kInt32),
          ConstantError::kInvalidShift);
    for (auto op : {kSlash, kPercent}) {
      error(test, cloth::binary_scalar(op, type, 1, 0, type),
            ConstantError::kZeroDivisor);
      if (is_signed)
        error(test, cloth::binary_scalar(op, type, minimum, mask, type),
              ConstantError::kOverflow);
    }
    if (is_signed) {
      error(test, cloth::binary_scalar(kMinus, type, minimum, 1, type),
            ConstantError::kOverflow);
      error(test, cloth::unary_scalar(kMinus, type, minimum),
            ConstantError::kOverflow);
      bits(test,
           cloth::binary_scalar(kShiftRight, type, minimum, width - 1, kInt32),
           mask);
    }
    bits(test,
         cloth::binary_scalar(kLess, type, is_signed ? minimum : 0, maximum,
                              type),
         1);
    bits(test, cloth::binary_scalar(kLessEqual, type, maximum, maximum, type),
         1);
    bits(test, cloth::binary_scalar(kGreater, type, maximum, 0, type), 1);
    bits(test,
         cloth::binary_scalar(kGreaterEqual, type, maximum, maximum, type), 1);
    bits(test, cloth::binary_scalar(kEqualEqual, type, maximum, maximum, type),
         1);
    bits(test, cloth::binary_scalar(kBangEqual, type, maximum, maximum, type),
         0);
  }
  for (const auto text : {"'\\q'", "'\\'", "'''", "'\n'", "'\r'"})
    error(test, cloth::scalar_literal(LiteralKind::kCharacter, text, kChar),
          ConstantError::kInvalidLiteral);
}

void conversions(TestContext& test) {
  using enum TypeKind;
  bits(test, cloth::convert_scalar(16777217, kInt32, kFloat32), 0x4b800000);
  bits(test, cloth::convert_scalar(16777219, kInt32, kFloat32), 0x4b800002);
  bits(test, cloth::convert_scalar(UINT64_MAX, kUint64, kFloat64),
       0x43f0000000000000);
  bits(test, cloth::convert_scalar(1, kFloat32, kFloat64), 0x36a0000000000000);
  bits(test, cloth::convert_scalar(0x8000000000000001, kFloat64, kFloat32),
       0x80000000);
  bits(test, cloth::convert_scalar(0x47efffffe0000000, kFloat64, kFloat32),
       0x7f7fffff);
  error(test, cloth::convert_scalar(0x47efffffe0000001, kFloat64, kFloat32),
        ConstantError::kOutOfRange);
  bits(test, cloth::convert_scalar(0xc060100000000000, kFloat64, kInt8),
       128);  // -128.5
  bits(test, cloth::convert_scalar(0xbfe0000000000000, kFloat64, kUint8),
       0);  // -0.5
  error(test, cloth::convert_scalar(0x4060000000000000, kFloat64, kInt8),
        ConstantError::kOutOfRange);
  error(test, cloth::convert_scalar(0x43f0000000000000, kFloat64, kUint64),
        ConstantError::kOutOfRange);
  bits(test, cloth::convert_scalar(255, kInt8, kInt64), UINT64_MAX);
  error(test, cloth::convert_scalar(255, kInt8, kUint8),
        ConstantError::kOutOfRange);
  bits(test, cloth::scalar_literal(LiteralKind::kFloat, "-1.9", kInt8), 255);
}

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string result;
  for (const auto& diagnostic : diagnostics.diagnostics())
    result += diagnostic.message + '\n';
  return result;
}

void add(cloth::Compilation& compilation, std::string name, std::string text) {
  compilation.add_source(
      cloth::SourceFile::from_memory(std::move(name), std::move(text)));
}

void check(TestContext& test, std::string source,
           std::string_view expected = {}) {
  cloth::Compilation compilation;
  add(compilation, "Example.co", std::move(source));
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  const auto detail = messages(diagnostics);
  test.expect(result.is_valid == expected.empty(),
              "frontend validity\n" + detail);
  if (!expected.empty())
    test.expect(detail.find(expected) != std::string::npos,
                "missing " + std::string{expected} + "\n" + detail);
  test.expect(detail.find("internal HIR") == std::string::npos,
              "internal error\n" + detail);
}

void frontend(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Example.co", R"(
    static final int64 Wide = Small + 2;
    static final int16 Small = 4;
    static final int32 Negative = -Wide;
    static final bool Skipped = false && (1 / 0 == 0);
  )");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  test.expect(!result.is_valid, "typed reference cannot implicitly narrow");
  check(test, R"(
    static final int64 Wide = Small + 2;
    static final int16 Small = 4;
    static final int64 Negative = -Wide;
    static final bool Skipped = false && (1 / 0 == 0);
    static final int32 Count = 32;
    static final bool AlsoSkipped = true || (1 << Count == 0);
    static final bool Compared = Wide >= Small && Negative != 0;
    static final char Letter = 'x';
    static final char Copy = Letter;
    static final bool Same = Letter == Copy;
    static final float Half = float(1) / 2.0;
    static final float32 BelowZero = -0.0;
    static final int64 Low = -9223372036854775808;
    static final int64 NestedLow = +(-9223372036854775808);
    static final int8 NestedSmall = int8(+(-128));
    static final bool SkippedOverflow = true || (-(-(-2147483648)) == 0);
    static final uint64 High = 18446744073709551615;
    static func Read(int64 n) { switch(n) { case Negative: {} } }
  )");
  for (const auto expression : {"2147483647 + 1", "-2147483648 / -1",
                                "-2147483648 % -1", "-(-(-2147483648))"})
    check(test, "static final int32 Bad = " + std::string{expression} + ";",
          "overflow");
  check(test, "static final int32 Bad = 1 / 0;", "by zero");
  check(test, "static final int8 Bad = int8(-(-(-128)));", "overflow");
  check(test, "static final int8 Bad = -(-(-128));", "overflow");
  check(test, "static final int64 Bad = -(-(-9223372036854775808));",
        "overflow");
  check(test, "static final int32 Bad = 1 << 32;", "shift count");
  check(test, "static final bool Bad = true && (1 / 0 == 0);", "by zero");
  check(test,
        "static final bool Bad = false && Probe(); static func Probe(): bool { "
        "return true; }",
        "scalar constant expression");
  check(test, "static final bool Bad = false && (1 == true);", "operator");
  check(test, "static final int32 Bad = int32(2147483648 + 0);",
        "out of range");
  check(test, "static final int64 Wide = 1; static final int32 Bad = Wide;",
        "expected 'int32'");
  check(test,
        "static final int32 Value = 4 * 1024; static func Read(int32 n) { "
        "switch(n) { case 4 * 1024: {} } }",
        "case label");
  check(test,
        "static final int32 Value = 4 * 1024; static func Read(int32 n) { "
        "switch(n) { case Value: {} } }");
  check(test,
        "static final int8 Negative = int8(-128); static func Read(int64 n) { "
        "switch(n) { case Negative: {} } }");
}

void dependencies_and_gates(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Example.co",
      "static final int32 Value = Other.Next + Other.Next; static func Main() "
      "{}");
  add(compilation, "Other.co",
      "static final int32 Next = Last + 1; static final int32 Last = 20;");
  cloth::DiagnosticEngine diagnostics;
  const auto frontend = compilation.analyze_frontend(diagnostics);
  test.expect(frontend.is_valid, messages(diagnostics));
  for (const auto& symbol : frontend.semantics.symbols()) {
    if (symbol.name == "Value")
      test.expect(symbol.static_constant && symbol.static_constant->bits == 42,
                  "memoized forward dependency");
  }
  const auto mir = cloth::lower_to_mir(frontend.hir, frontend.semantics);
  cloth::DiagnosticEngine mir_errors;
  test.expect(cloth::verify_mir(mir, frontend.semantics, mir_errors),
              messages(mir_errors));
  for (const auto& file : mir.files)
    for (const auto& field : file.fields)
      test.expect(field.static_constant && !field.initializer,
                  "static fields contain data, not executable bodies");
  cloth::DiagnosticEngine emission_errors;
  test.expect(compilation.analyze(emission_errors).is_valid,
              messages(emission_errors));
  cloth::Compilation inherited;
  add(inherited, "Base.co", "static final int32 Value = 6 * 7;");
  add(inherited, "Child.co",
      "class : Base { static final int32 Copy = Child.Value; }");
  cloth::DiagnosticEngine inheritance_errors;
  test.expect(inherited.analyze(inheritance_errors).is_valid,
              messages(inheritance_errors));
  check(test, "static final uint64 Zero = uint64(0) - uint64(0);");
  check(test, "static final bool Self = false && Self;",
        "cyclic static constant");
  check(test, "static final int32 A = B; static final int32 B = A;",
        "cyclic static constant");
  check(test, "static final int32 unused = 1 / 0;", "by zero");
  cloth::Compilation private_reference;
  add(private_reference, "Example.co",
      "static final int32 Value = Other.hidden;");
  add(private_reference, "Other.co", "static final int32 hidden = 1;");
  cloth::DiagnosticEngine privacy;
  test.expect(!private_reference.analyze_frontend(privacy).is_valid,
              "private constants remain private");
  test.expect(messages(privacy).find("private") != std::string::npos,
              "privacy diagnostic");
}

void constant_claims(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Example.co", R"(
    static final int8 Small = int8(-128);
    static final bool Enabled = true;
    static final char Letter = 'A';
    static final float32 Half = 1.0 / 2.0;
    static final float64 Wide = 1.5;
    static final State Initial = State.Ready;
    static final int32 Value = 6 * 7;
  )");
  add(compilation, "State.co", "enum { Ready }");
  add(compilation, "Other.co", "enum { Ready }");
  cloth::DiagnosticEngine diagnostics;
  const auto valid = compilation.analyze(diagnostics);
  test.expect(valid.is_valid, messages(diagnostics));
  if (!valid.is_valid) return;
  const std::pair<std::string_view, std::uint64_t> bad_bits[]{
      {"Small", 256},       {"Enabled", 2},        {"Letter", 256},
      {"Half", 0x7f800000}, {"Half", 0x100000000}, {"Wide", 0x7ff8000000000000},
      {"Initial", 1},       {"Value", 41}};
  for (const auto& [name, bits] : bad_bits) {
    auto hir = valid.hir;
    auto mir = valid.mir;
    auto semantics = valid.semantics;
    for (auto& file : hir.files) {
      for (auto& field : file.fields) {
        if (semantics.symbol(field.symbol).name != name) continue;
        field.static_constant->bits = bits;
        // Corrupt both copies. HIR must check the expression, not just
        // equality.
        const_cast<cloth::SemanticSymbol&>(semantics.symbol(field.symbol))
            .static_constant->bits = bits;
      }
    }
    for (auto& file : mir.files)
      for (auto& field : file.fields)
        if (valid.semantics.symbol(field.symbol).name == name)
          field.static_constant->bits = bits;
    cloth::DiagnosticEngine hir_errors;
    test.expect(!cloth::verify_hir(hir, semantics, hir_errors),
                "HIR accepted invalid claim for " + std::string{name});
    cloth::DiagnosticEngine mir_errors;
    test.expect(!cloth::verify_mir(mir, valid.semantics, mir_errors),
                "MIR accepted invalid claim for " + std::string{name});
  }
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto hir = valid.hir;
    auto mir = valid.mir;
    auto& h = hir.files[0].fields[0];
    auto& m = mir.files[0].fields[0];
    if (mutation == 0) {
      h.static_constant.reset();
      m.static_constant.reset();
    }
    if (mutation == 1) {
      h.static_constant->type = valid.semantics.bool_type();
      m.static_constant->type = valid.semantics.bool_type();
    }
    if (mutation == 2) {
      h.initializer.reset();
      m.initializer = cloth::MirBody{};
    }
    if (mutation == 3) {
      h.symbol = hir.files[0].fields[1].symbol;
      m.symbol = mir.files[0].fields[1].symbol;
    }
    cloth::DiagnosticEngine he, me;
    test.expect(!cloth::verify_hir(hir, valid.semantics, he),
                "HIR missing/type/body/identity claim");
    test.expect(!cloth::verify_mir(mir, valid.semantics, me),
                "MIR missing/type/body/identity claim");
  }
  {
    auto hir = valid.hir;
    auto& field = hir.files[0].fields.back();
    auto& expression = const_cast<cloth::HirExpression&>(
        hir.storage.expression(*field.initializer));
    expression.data = cloth::HirSymbolExpression{field.symbol};
    cloth::DiagnosticEngine errors;
    test.expect(!cloth::verify_hir(hir, valid.semantics, errors) &&
                    messages(errors).find("cyclic") != std::string::npos,
                "HIR accepted a self-dependent constant claim");
  }
  {
    auto hir = valid.hir;
    auto& field = hir.files[0].fields[5];
    field.static_constant->type = *valid.semantics.find_type("Other");
    cloth::DiagnosticEngine errors;
    test.expect(!cloth::verify_hir(hir, valid.semantics, errors),
                "HIR accepted a wrong-owner enum claim");
  }
  // Export a package-owned fixture for independent imported-model checks.
  cloth::Compilation package;
  package.add_package_source(
      cloth::SourceFile::from_memory(
          "Values.co",
          "static final float32 Half = 0.5; static final bool On = true;"),
      "library", "", "1.0.0");
  cloth::DiagnosticEngine pe;
  auto result = package.analyze(pe);
  auto view = cloth::build_imported_package_view(
      {"library", "1.0.0"}, result.semantics, result.mir, result.abi);
  test.expect(view.is_valid(), "constant view fixture");
  if (!view.view) return;
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto broken = *view.view;
    auto& member = *std::ranges::find(broken.files[0].members, "Half",
                                      &cloth::ImportedMember::name);
    if (mutation == 0) member.static_value.reset();
    if (mutation == 1) member.static_value->kind = TypeKind::kInt32;
    if (mutation == 2) member.static_value->bits = 0x7fc00000;
    if (mutation == 3) member.static_value->bits = 0x100000000;
    if (mutation == 4) member.is_static = false;
    test.expect(!cloth::verify_imported_package_view(broken).empty(),
                "imported verifier accepted a malformed constant");
  }
  auto excessive = *view.view;
  excessive.files[0].members.resize(cloth::kMaxStaticConstants + 1,
                                    excessive.files[0].members.front());
  const auto issues = cloth::verify_imported_package_view(excessive);
  test.expect(std::ranges::any_of(
                  issues,
                  [](const auto& issue) {
                    return issue.code ==
                           cloth::ImportedPackageIssueCode::kLimitExceeded;
                  }),
              "imported constant count has a typed resource failure");
}

void malformed_constant_trees(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Values.co",
      "static final int32 Value = +(-42); static final char Letter = 'q';");
  cloth::DiagnosticEngine errors;
  const auto valid = compilation.analyze(errors);
  test.expect(valid.is_valid, messages(errors));
  if (!valid.is_valid) return;
  for (int mutation = 0; mutation < 7; ++mutation) {
    auto hir = valid.hir;
    auto& field = hir.files[0].fields[0];
    auto& expression = const_cast<cloth::HirExpression&>(
        hir.storage.expression(*field.initializer));
    if (mutation == 0)
      std::get<cloth::HirUnaryExpression>(expression.data)
          .operand_is_presence_test = true;
    if (mutation == 1) {
      const auto child =
          std::get<cloth::HirUnaryExpression>(expression.data).operand;
      const_cast<cloth::HirExpression&>(hir.storage.expression(child)).type =
          valid.semantics.bool_type();
    }
    if (mutation == 2)
      expression.data = cloth::HirGroupedExpression{*field.initializer};
    if (mutation == 3)
      expression.data = cloth::HirLiteralExpression{LiteralKind::kInteger,
                                                    std::string(4097, '0')};
    if (mutation == 4)
      hir.files[0].fields.resize(cloth::kMaxStaticConstants + 1, field);
    if (mutation == 5) {
      auto id = *field.initializer;
      for (int i = 0; i < 17; ++i) {
        auto node = hir.storage.expression(id);
        node.data = cloth::HirBinaryExpression{id, TokenKind::kPlus, id};
        id = hir.storage.add_expression(std::move(node));
      }
      field.initializer = id;
    }
    if (mutation == 6) {
      auto& character = const_cast<cloth::HirExpression&>(
          hir.storage.expression(*hir.files[0].fields[1].initializer));
      character.data =
          cloth::HirLiteralExpression{LiteralKind::kCharacter, "'\\q'"};
    }
    cloth::DiagnosticEngine rejected;
    test.expect(!cloth::verify_hir(hir, valid.semantics, rejected),
                "malformed constant tree " + std::to_string(mutation));
  }
}

void imported_constants(TestContext& test) {
  for (const auto& target : {cloth::TargetDataLayout::llvm_x86_64(),
                             cloth::TargetDataLayout::llvm_wasm32()}) {
    cloth::Compilation producer{target};
    producer.add_package_source(
        cloth::SourceFile::from_memory(
            "Constants.co",
            "static final bool Enabled = true; static final char Letter = "
            "'\\n'; "
            "static final float32 Half = 0.5; static final float64 Wide = 1.5; "
            "static final int16 Small = 4; static final State Initial = "
            "State.Ready;"),
        "library", "", "1.0.0");
    producer.add_package_source(
        cloth::SourceFile::from_memory("State.co", "enum { Ready, Done }"),
        "library", "", "1.0.0");
    cloth::DiagnosticEngine errors;
    const auto result = producer.analyze(errors);
    test.expect(result.is_valid, messages(errors));
    if (!result.is_valid) continue;
    auto exported = cloth::build_imported_package_view(
        {"library", "1.0.0"}, result.semantics, result.mir, result.abi);
    test.expect(exported.is_valid(), "exported scalar constants");
    if (!exported.view) continue;
    cloth::Compilation consumer{target};
    consumer.set_package_dependencies({{"app", "dep", "library"}});
    consumer.add_imported_package(*exported.view);
    consumer.add_package_source(cloth::SourceFile::from_memory("Example.co", R"(
      import dep::Constants as Values;
      import dep::State as Status;
      static final bool Enabled = Values.Enabled && Values.Letter == '\n';
      static final float32 Product = Values.Half * Values.Half;
      static final float64 Wide = Values.Wide + Values.Half;
      static final int64 Count = Values.Small + 2;
      static final Status State = Values.Initial;
      static final bool Same = State == Status.Ready;
    )"),
                                "app", "", "1.0.0");
    cloth::DiagnosticEngine diagnostics;
    const auto checked = consumer.analyze(diagnostics);
    test.expect(checked.is_valid, messages(diagnostics));
    const std::pair<std::string_view, std::uint64_t> expected[]{
        {"Enabled", 1},
        {"Product", 0x3e800000},
        {"Wide", 0x4000000000000000},
        {"Count", 6},
        {"State", 0},
        {"Same", 1}};
    for (const auto& [name, value] : expected) {
      const auto found = std::ranges::find_if(
          checked.semantics.symbols(), [&](const auto& symbol) {
            return symbol.kind == cloth::SymbolKind::kField &&
                   symbol.name == name &&
                   checked.semantics.file(*symbol.file).identity.package.name ==
                       "app";
          });
      test.expect(found != checked.semantics.symbols().end() &&
                      found->static_constant &&
                      found->static_constant->bits == value,
                  "imported value " + std::string{name});
    }
  }
}

void cross_target_constants(TestContext& test) {
  const std::pair<std::string_view, std::uint64_t> expected[]{
      {"I8", 128},
      {"I16", 32768},
      {"I32", 2147483648},
      {"I64", UINT64_C(0x8000000000000000)},
      {"U8", 255},
      {"Byte", 255},
      {"U16", 65535},
      {"U32", 4294967295},
      {"U64", UINT64_MAX},
      {"Char", 0},
      {"Bool", 1},
      {"Third", 0x3eaaaaab},
      {"Wide", 0x3fd5555560000000},
      {"Quotient", 0x3fd5555555555555},
      {"Zero", 0x80000000},
      {"Tiny", 1},
      {"Underflow", 0},
      {"Rounded", 0x3f800000},
      {"Initial", 1}};
  for (const auto& target : {cloth::TargetDataLayout::llvm_x86_64(),
                             cloth::TargetDataLayout::llvm_wasm32()}) {
    cloth::Compilation compilation{target};
    add(compilation, "Values.co", R"(
      static final int8 I8 = int8(-128);
      static final int16 I16 = -32767 - 1;
      static final int32 I32 = -2147483647 - 1;
      static final int64 I64 = -9223372036854775807 - 1;
      static final uint8 U8 = ~uint8(0);
      static final byte Byte = ~byte(0);
      static final uint16 U16 = ~uint16(0);
      static final uint32 U32 = ~uint32(0);
      static final uint64 U64 = ~uint64(0);
      static final char Char = '\0';
      static final bool Bool = I8 < 0 && U64 != 0;
      static final float32 Third = 1.0 / 3.0;
      static final float64 Wide = Third;
      static final float64 Quotient = 1.0 / 3.0;
      static final float32 Zero = -0.0 * 2.0;
      static final float32 Tiny = 0.000000000000000000000000000000000000000000001;
      static final float32 Underflow = Tiny / 2.0;
      static final float32 Rounded = 1.0 + 0.000000059604644775390625;
      static final State Initial = State.Done;
    )");
    add(compilation, "State.co", "enum { Ready, Done }");
    cloth::DiagnosticEngine errors;
    const auto result = compilation.analyze(errors);
    test.expect(result.is_valid, messages(errors));
    if (!result.is_valid) continue;
    for (const auto& [name, value] : expected) {
      const auto found = std::ranges::find(result.semantics.symbols(), name,
                                           &cloth::SemanticSymbol::name);
      test.expect(found != result.semantics.symbols().end() &&
                      found->static_constant &&
                      found->static_constant->bits == value,
                  "target-independent exact bits for " + std::string{name});
    }
    const auto ir =
        cloth::emit_llvm_ir(result.mir, result.abi, result.semantics, errors);
    test.expect(ir && !ir->text.contains("llvm.global_ctors"),
                "constant globals need no runtime initializer");
  }
}

void deterministic_failures(TestContext& test) {
  std::string baseline;
  for (const bool reverse : {false, true}) {
    cloth::Compilation compilation;
    const auto add_first = [&] {
      add(compilation, "First.co",
          "static final int32 A = Last.Z; static final int32 Dependent = A; ");
    };
    const auto add_last = [&] {
      add(compilation, "Last.co",
          "static final int32 Z = First.A; static final int32 Failure = 1 / "
          "0;");
    };
    if (reverse) {
      add_last();
      add_first();
    } else {
      add_first();
      add_last();
    }
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze_frontend(diagnostics);
    test.expect(!result.is_valid, "invalid dependency graph");
    std::string detail;
    std::size_t error_count = 0;
    for (const auto& diagnostic : diagnostics.diagnostics()) {
      detail += diagnostic.message + '\n';
      if (diagnostic.severity == cloth::DiagnosticSeverity::kError)
        ++error_count;
    }
    test.expect(
        error_count == 2,
        "originating failures only, without dependent cascades\n" + detail);
    for (const auto& symbol : result.semantics.symbols())
      if (symbol.kind == cloth::SymbolKind::kField)
        test.expect(!symbol.static_constant,
                    "failed dependency did not become zero");
    if (!reverse)
      baseline = detail;
    else
      test.expect(detail == baseline,
                  "canonical dependency diagnostics\n" + detail);
  }
  std::string cycle;
  for (int i = 0; i < 12; ++i)
    cycle += "static final int32 N" + std::to_string(i) + " = N" +
             std::to_string((i + 1) % 12) + ";";
  cloth::Compilation compilation;
  add(compilation, "Cycle.co", std::move(cycle));
  cloth::DiagnosticEngine diagnostics;
  test.expect(!compilation.analyze_frontend(diagnostics).is_valid,
              "long cycle rejected");
  test.expect(
      diagnostics.diagnostics().size() == 10 &&
          messages(diagnostics).find("4 additional cycle references omitted") !=
              std::string::npos,
      "bounded cycle path\n" + messages(diagnostics));
  std::string diamond =
      "static final bool N0 = true; static final bool N1 = true;";
  for (int i = 2; i < 1000; ++i)
    diamond += "static final bool N" + std::to_string(i) + " = N" +
               std::to_string(i - 1) + " && N" + std::to_string(i - 2) + ";";
  check(test, std::move(diamond));
}

std::string balanced_sum(std::size_t leaves) {
  if (leaves == 1) return "1";
  return "(" + balanced_sum(leaves / 2) + "+" +
         balanced_sum(leaves - leaves / 2) + ")";
}

void resource_limits(TestContext& test) {
  std::string additions = "1";
  std::string conjunctions = "true";
  std::string conversions = "1";
  for (int i = 1; i < 256; ++i) {
    additions += "+1";
    conjunctions += "&&true";
    conversions = "int32(" + conversions + ")";
  }
  check(test, "static final int32 Value = " + additions + ";");
  check(test, "static final bool Value = " + conjunctions + ";");
  check(test, "static final int32 Value = " + conversions + ";");
  check(test, "static final int32 Value = " + additions + "+1;",
        "nesting depth 256");
  check(test, "static final int32 Value = int32(" + conversions + ");",
        "nesting depth 256");
  std::string calls = "1";
  for (int i = 1; i < 256; ++i) calls = "Probe(" + calls + ")";
  check(test,
        "static final int32 Value = " + calls +
            "; static func Probe(int32 value): int32 { return value; }",
        "scalar constant expression");
  std::string members = "Other";
  for (int i = 1; i < 256; ++i) members += ".Value";
  check(test, "static final int32 Value = " + members + ";",
        "scalar constant expression");
  check(test, "static final int32 Value = " + std::string(255, '(') + "1" +
                  std::string(255, ')') + ";");
  check(test,
        "static final int32 Value = " + std::string(256, '(') + "1" +
            std::string(256, ')') + ";",
        "nesting depth 256");
  check(test, "static final bool Value = " + std::string(255, '!') + "true;");
  check(test, "static final bool Value = " + std::string(256, '!') + "true;",
        "nesting depth 256");
  check(test, "static final int32 Value = " + std::string(4095, '0') + "1;");
  check(test, "static final int32 Value = " + std::string(4096, '0') + "1;",
        "4096 bytes");
  check(test,
        "static final int32 Value = " + std::string(10000, '(') + "1" +
            std::string(10000, ')') + ";",
        "nesting depth 256");
  std::string chain;
  for (std::size_t i = 0; i < cloth::kMaxStaticConstants; ++i)
    chain +=
        "static final int32 N" + std::to_string(i) + " = " +
        (i + 1 == cloth::kMaxStaticConstants ? "1"
                                             : "N" + std::to_string(i + 1)) +
        ";\n";
  {
    cloth::Compilation compilation;
    add(compilation, "Chain.co", chain);
    cloth::DiagnosticEngine errors;
    auto result = compilation.analyze_frontend(errors);
    test.expect(result.is_valid, messages(errors));
    if (result.is_valid) {
      const auto mir = cloth::lower_to_mir(result.hir, result.semantics);
      test.expect(cloth::verify_mir(mir, result.semantics, errors),
                  messages(errors));
      // Turn the file symbol into an extra constant claim. The count guard
      // must reject it before downstream consumers inspect the malformed file.
      const auto field =
          result.semantics.symbol(result.hir.files[0].fields[0].symbol);
      const_cast<cloth::SemanticSymbol&>(
          result.semantics.symbol(result.hir.files[0].symbol)) = field;
      cloth::DiagnosticEngine rejected;
      test.expect(!cloth::verify_mir(mir, result.semantics, rejected) &&
                      messages(rejected).contains("65536 static constants"),
                  "MIR constant count is bounded");
    }
  }
  check(test, chain + "static final int32 Extra = 1;",
        "65536 static constants");
  // Each binary node has a source grouping node: 3 * leaves - 2 nodes.
  const auto expression = balanced_sum(21846);
  check(test, "static final int32 Value = " + expression + ";");
  check(test, "static final int32 Value = (" + expression + ");",
        "65536 expression nodes");
  std::string package;
  for (int i = 0; i < 16; ++i)
    package +=
        "static final int32 N" + std::to_string(i) + " = " + expression + ";";
  check(test, package);
  check(test, package + "static final int32 Extra = 1;",
        "1048576 constant expression nodes");
  // Package budgets do not accumulate across independent owning packages.
  cloth::Compilation multiple;
  multiple.add_package_source(cloth::SourceFile::from_memory("First.co", chain),
                              "first", "", "1.0.0");
  multiple.add_package_source(
      cloth::SourceFile::from_memory("Second.co", "static final int32 N = 1;"),
      "second", "", "1.0.0");
  cloth::DiagnosticEngine diagnostics;
  test.expect(multiple.analyze_frontend(diagnostics).is_valid,
              messages(diagnostics));
  cloth::Compilation same_package;
  same_package.add_package_source(
      cloth::SourceFile::from_memory("First.co", chain), "app", "", "1.0.0");
  same_package.add_package_source(
      cloth::SourceFile::from_memory("Second.co", "static final int32 N = 1;"),
      "app", "other", "1.0.0");
  cloth::DiagnosticEngine exceeded;
  test.expect(!same_package.analyze_frontend(exceeded).is_valid &&
                  messages(exceeded).find("65536 static constants") !=
                      std::string::npos,
              "budget covers source directories within one owning package");
}
}  // namespace

int main() {
  constexpr TestCase tests[]{
      {"integer values", integer_values},
      {"floating vectors", floating_vectors},
      {"scalar width boundaries", scalar_boundaries},
      {"checked conversions", conversions},
      {"constant frontend", frontend},
      {"dependencies and lowering", dependencies_and_gates},
      {"constant claims", constant_claims},
      {"malformed constant trees", malformed_constant_trees},
      {"imported scalar values", imported_constants},
      {"cross-target exact constant bits", cross_target_constants},
      {"deterministic graph failures", deterministic_failures},
      {"resource limits", resource_limits},
  };
  return cloth::test::run_tests(tests);
}
