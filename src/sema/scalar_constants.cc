// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/scalar_constants.h"

#include "cloth/lexer/literal.h"
#include "cloth/sema/numeric_types.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace cloth {
namespace {

// Unsigned base-2^32 arithmetic for bounded decimal conversion and IEEE
// rounding. No host floating-point operations or extended integer types are
// used. Inputs are bounded by the constant-expression resource contract.
class Natural {
 public:
  explicit Natural(std::uint64_t value = 0) {
    if (value) words_.push_back(static_cast<std::uint32_t>(value));
    if (value >> 32) words_.push_back(static_cast<std::uint32_t>(value >> 32));
  }
  bool zero() const { return words_.empty(); }
  int width() const {
    return zero() ? 0
                  : static_cast<int>((words_.size() - 1) * 32) + 32 -
                        std::countl_zero(words_.back());
  }
  std::uint64_t low() const {
    return zero()
               ? 0
               : words_[0] |
                     (words_.size() > 1 ? std::uint64_t{words_[1]} << 32 : 0);
  }
  int compare(const Natural& other) const {
    if (words_.size() != other.words_.size())
      return words_.size() < other.words_.size() ? -1 : 1;
    for (std::size_t i = words_.size(); i > 0; --i)
      if (words_[i - 1] != other.words_[i - 1])
        return words_[i - 1] < other.words_[i - 1] ? -1 : 1;
    return 0;
  }
  void shift(int bits) {
    assert(bits >= 0);
    if (zero() || bits == 0) return;
    const auto whole = static_cast<std::size_t>(bits / 32);
    const unsigned part = static_cast<unsigned>(bits % 32);
    words_.insert(words_.begin(), whole, 0);
    std::uint64_t carry = 0;
    for (std::size_t i = whole; i < words_.size(); ++i) {
      const std::uint64_t value = (std::uint64_t{words_[i]} << part) | carry;
      words_[i] = static_cast<std::uint32_t>(value);
      carry = value >> 32;
    }
    if (carry) words_.push_back(static_cast<std::uint32_t>(carry));
  }
  void add(const Natural& other) {
    words_.resize(std::max(words_.size(), other.words_.size()), 0);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < words_.size(); ++i) {
      const std::uint64_t value =
          std::uint64_t{words_[i]} + carry +
          (i < other.words_.size() ? other.words_[i] : 0);
      words_[i] = static_cast<std::uint32_t>(value);
      carry = value >> 32;
    }
    if (carry) words_.push_back(static_cast<std::uint32_t>(carry));
  }
  void subtract(const Natural& other) {
    assert(compare(other) >= 0);
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < words_.size(); ++i) {
      const std::uint64_t sub =
          borrow + (i < other.words_.size() ? other.words_[i] : 0);
      const std::uint64_t value = words_[i];
      words_[i] = static_cast<std::uint32_t>(value - sub);
      borrow = value < sub;
    }
    while (!words_.empty() && words_.back() == 0) words_.pop_back();
  }
  Natural multiply(const Natural& other) const {
    Natural result;
    if (zero() || other.zero()) return result;
    result.words_.resize(words_.size() + other.words_.size());
    for (std::size_t i = 0; i < words_.size(); ++i) {
      std::uint64_t carry = 0;
      for (std::size_t j = 0; j < other.words_.size(); ++j) {
        const std::uint64_t value = std::uint64_t{words_[i]} * other.words_[j] +
                                    result.words_[i + j] + carry;
        result.words_[i + j] = static_cast<std::uint32_t>(value);
        carry = value >> 32;
      }
      result.words_[i + other.words_.size()] =
          static_cast<std::uint32_t>(carry);
    }
    while (!result.words_.empty() && result.words_.back() == 0)
      result.words_.pop_back();
    return result;
  }

 private:
  std::vector<std::uint32_t> words_;
};

struct FloatFormat {
  int fraction;
  int exponent;
  int bias;
};

FloatFormat format(TypeKind type) {
  assert(type == TypeKind::kFloat32 || type == TypeKind::kFloat64);
  return type == TypeKind::kFloat32 ? FloatFormat{23, 8, 127}
                                    : FloatFormat{52, 11, 1023};
}

bool floating(TypeKind type) {
  return type == TypeKind::kFloat32 || type == TypeKind::kFloat64;
}

std::uint64_t sign_bit(TypeKind type) {
  const auto f = format(type);
  return std::uint64_t{1} << (f.fraction + f.exponent);
}

std::uint64_t mask(int width) {
  return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
}

std::uint64_t mask(std::uint32_t width) {
  return mask(static_cast<int>(width));
}

struct Finite {
  bool negative;
  Natural magnitude;
  int scale;
};

Finite unpack(std::uint64_t bits, TypeKind type) {
  const auto f = format(type);
  const int exponent =
      static_cast<int>((bits >> f.fraction) & mask(f.exponent));
  return Finite{(bits & sign_bit(type)) != 0,
                Natural{(bits & mask(f.fraction)) |
                        (exponent ? std::uint64_t{1} << f.fraction : 0)},
                (exponent ? exponent : 1) - f.bias - f.fraction};
}

int scaled_compare(Natural left, Natural right, int difference) {
  if (difference >= 0)
    left.shift(difference);
  else
    right.shift(-difference);
  return left.compare(right);
}

// Round numerator / denominator * 2^scale directly to the destination format.
ConstantBits pack(bool negative, Natural numerator, Natural denominator,
                  int scale, TypeKind type, bool literal = false) {
  const auto f = format(type);
  const std::uint64_t sign = negative ? sign_bit(type) : 0;
  if (numerator.zero()) return sign;
  assert(!denominator.zero());
  int exponent = numerator.width() - denominator.width() + scale;
  if (scaled_compare(numerator, denominator, scale - exponent) < 0) --exponent;
  if (exponent > f.bias) return std::unexpected(ConstantError::kNonFinite);
  if (exponent < 1 - f.bias - f.fraction - 1)
    return literal ? ConstantBits{std::unexpected(ConstantError::kOutOfRange)}
                   : ConstantBits{sign};
  const int quantum = std::max(exponent, 1 - f.bias) - f.fraction;
  const int shift = scale - quantum;
  if (shift >= 0)
    numerator.shift(shift);
  else
    denominator.shift(-shift);
  std::uint64_t quotient = 0;
  for (int bit = numerator.width() - denominator.width(); bit >= 0; --bit) {
    assert(bit < 64);
    Natural divisor = denominator;
    divisor.shift(bit);
    if (numerator.compare(divisor) >= 0) {
      numerator.subtract(divisor);
      quotient |= std::uint64_t{1} << bit;
    }
  }
  numerator.shift(1);
  const int halfway = numerator.compare(denominator);
  if (halfway > 0 || (halfway == 0 && (quotient & 1))) ++quotient;
  if (!quotient)
    return literal ? ConstantBits{std::unexpected(ConstantError::kOutOfRange)}
                   : ConstantBits{sign};
  if (quotient == (std::uint64_t{1} << (f.fraction + 1))) {
    quotient >>= 1;
    ++exponent;
  }
  if (exponent > f.bias) return std::unexpected(ConstantError::kNonFinite);
  const bool normal = quotient >= (std::uint64_t{1} << f.fraction);
  const auto stored_exponent =
      normal ? std::max(exponent, 1 - f.bias) + f.bias : 0;
  return sign |
         (std::uint64_t{static_cast<unsigned>(stored_exponent)} << f.fraction) |
         (quotient & mask(f.fraction));
}

ConstantBits decimal_float(std::string_view text, bool negative,
                           TypeKind type) {
  Natural numerator;
  Natural denominator{1};
  bool fractional = false;
  bool digit = false;
  for (char ch : text) {
    if (ch == '.' && !fractional) {
      fractional = true;
      continue;
    }
    if (ch < '0' || ch > '9')
      return std::unexpected(ConstantError::kInvalidLiteral);
    digit = true;
    numerator = numerator.multiply(Natural{10});
    numerator.add(Natural{static_cast<std::uint64_t>(ch - '0')});
    if (fractional) denominator = denominator.multiply(Natural{10});
  }
  if (!digit) return std::unexpected(ConstantError::kInvalidLiteral);
  return pack(negative, std::move(numerator), std::move(denominator), 0, type,
              true);
}

struct Integer {
  bool negative;
  std::uint64_t magnitude;
};

Integer integer(std::uint64_t bits, TypeKind type) {
  const auto p = *numeric_type_properties(type);
  const bool negative = p.category == NumericCategory::kSignedInteger &&
                        (bits & (std::uint64_t{1} << (p.bit_width - 1)));
  return {negative,
          negative ? (std::uint64_t{0} - bits) & mask(p.bit_width) : bits};
}

ConstantBits integer_bits(Integer value, TypeKind type) {
  const auto p = *numeric_type_properties(type);
  const bool signed_type = p.category == NumericCategory::kSignedInteger;
  const auto maximum =
      signed_type ? (std::uint64_t{1} << (p.bit_width - 1)) - !value.negative
                  : mask(p.bit_width);
  if ((!signed_type && value.negative && value.magnitude) ||
      value.magnitude > maximum)
    return std::unexpected(ConstantError::kOutOfRange);
  return (value.negative ? std::uint64_t{0} - value.magnitude
                         : value.magnitude) &
         mask(p.bit_width);
}

bool comparison(TokenKind operation, int order) {
  switch (operation) {
    case TokenKind::kEqualEqual:
      return order == 0;
    case TokenKind::kBangEqual:
      return order != 0;
    case TokenKind::kLess:
      return order < 0;
    case TokenKind::kLessEqual:
      return order <= 0;
    case TokenKind::kGreater:
      return order > 0;
    case TokenKind::kGreaterEqual:
      return order >= 0;
    default:
      return false;
  }
}

bool is_comparison(TokenKind operation) {
  return operation == TokenKind::kEqualEqual ||
         operation == TokenKind::kBangEqual || operation == TokenKind::kLess ||
         operation == TokenKind::kLessEqual ||
         operation == TokenKind::kGreater ||
         operation == TokenKind::kGreaterEqual;
}

ConstantBits float_binary(TokenKind operation, TypeKind type,
                          std::uint64_t left, std::uint64_t right) {
  auto a = unpack(left, type);
  auto b = unpack(right, type);
  if (is_comparison(operation)) {
    int order = scaled_compare(a.magnitude, b.magnitude, a.scale - b.scale);
    if (a.magnitude.zero() && b.magnitude.zero())
      order = 0;
    else if (a.negative != b.negative)
      order = a.negative ? -1 : 1;
    else if (a.negative)
      order = -order;
    return comparison(operation, order);
  }
  if (operation == TokenKind::kStar)
    return pack(a.negative != b.negative, a.magnitude.multiply(b.magnitude),
                Natural{1}, a.scale + b.scale, type);
  if (operation == TokenKind::kSlash) {
    if (b.magnitude.zero()) return std::unexpected(ConstantError::kZeroDivisor);
    return pack(a.negative != b.negative, std::move(a.magnitude),
                std::move(b.magnitude), a.scale - b.scale, type);
  }
  if (operation != TokenKind::kPlus && operation != TokenKind::kMinus)
    return std::unexpected(ConstantError::kInvalidOperation);
  if (operation == TokenKind::kMinus) b.negative = !b.negative;
  const int scale = std::min(a.scale, b.scale);
  a.magnitude.shift(a.scale - scale);
  b.magnitude.shift(b.scale - scale);
  if (a.negative == b.negative)
    a.magnitude.add(b.magnitude);
  else {
    const int order = a.magnitude.compare(b.magnitude);
    if (order < 0) std::swap(a, b);
    a.magnitude.subtract(b.magnitude);
    if (a.magnitude.zero()) a.negative = false;
  }
  return pack(a.negative, std::move(a.magnitude), Natural{1}, scale, type);
}

}  // namespace

std::string_view constant_error_message(ConstantError error) {
  switch (error) {
    case ConstantError::kInvalidLiteral:
      return "invalid scalar constant literal";
    case ConstantError::kOutOfRange:
      return "constant value is out of range for its type";
    case ConstantError::kOverflow:
      return "constant arithmetic overflow";
    case ConstantError::kZeroDivisor:
      return "constant division or remainder by zero";
    case ConstantError::kInvalidShift:
      return "constant shift count is out of range";
    case ConstantError::kNonFinite:
      return "constant floating-point result is not finite";
    case ConstantError::kInvalidOperation:
      return "invalid scalar constant operation";
  }
  return "invalid scalar constant";
}

bool is_scalar_constant_type(TypeKind type) {
  return is_numeric_type(type) || type == TypeKind::kBool ||
         type == TypeKind::kChar || type == TypeKind::kEnum;
}

bool is_valid_scalar_bits(TypeKind type, std::uint64_t bits) {
  if (is_integer_type(type)) return is_valid_integer_bits(bits, type);
  if (type == TypeKind::kBool) return bits <= 1;
  if (type == TypeKind::kChar) return bits <= 255;
  if (type == TypeKind::kEnum) return bits < 65536;
  if (!floating(type)) return false;
  const auto f = format(type);
  return (bits & ~mask(f.fraction + f.exponent + 1)) == 0 &&
         ((bits >> f.fraction) & mask(f.exponent)) != mask(f.exponent);
}

ConstantBits scalar_literal(LiteralKind literal, std::string_view text,
                            TypeKind target, bool negative) {
  if (text.empty() || text.size() > kMaxConstantLiteralBytes)
    return std::unexpected(ConstantError::kInvalidLiteral);
  if (literal == LiteralKind::kBoolean && target == TypeKind::kBool) {
    if (text == "true") return 1;
    if (text == "false") return 0;
  }
  if (literal == LiteralKind::kCharacter && target == TypeKind::kChar &&
      text.front() == '\'' && text.back() == '\'' &&
      ((text.size() == 3 && text[1] != '\\' && text[1] != '\'' &&
        text[1] != '\r' && text[1] != '\n') ||
       (text.size() == 4 && text[1] == '\\' &&
        std::string_view{"nrt0\\\"'"}.contains(text[2]))))
    return static_cast<unsigned char>(
        text[1] == '\\' ? decode_escape_character(text[2]) : text[1]);
  if (text.front() == '-') {
    negative = !negative;
    text.remove_prefix(1);
  }
  if (text.empty()) return std::unexpected(ConstantError::kInvalidLiteral);
  if (literal == LiteralKind::kFloat && is_numeric_type(target)) {
    const auto value = decimal_float(
        text, negative, floating(target) ? target : TypeKind::kFloat64);
    if (!value || floating(target)) return value;
    return convert_scalar(*value, TypeKind::kFloat64, target);
  }
  if (literal == LiteralKind::kInteger && is_numeric_type(target)) {
    std::uint64_t magnitude = 0;
    for (char ch : text) {
      if (ch < '0' || ch > '9')
        return std::unexpected(ConstantError::kInvalidLiteral);
      const auto digit = static_cast<std::uint64_t>(ch - '0');
      if (magnitude > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        return std::unexpected(ConstantError::kOutOfRange);
      magnitude = magnitude * 10 + digit;
    }
    return floating(target)
               ? pack(negative, Natural{magnitude}, Natural{1}, 0, target)
               : integer_bits({negative, magnitude}, target);
  }
  return std::unexpected(ConstantError::kInvalidLiteral);
}

ConstantBits convert_scalar(std::uint64_t bits, TypeKind source,
                            TypeKind target) {
  if (!is_valid_scalar_bits(source, bits))
    return std::unexpected(ConstantError::kOutOfRange);
  if (source == target) return bits;
  if (!is_numeric_type(source) || !is_numeric_type(target))
    return std::unexpected(ConstantError::kInvalidOperation);
  if (!floating(source)) {
    const auto value = integer(bits, source);
    return floating(target) ? pack(value.negative, Natural{value.magnitude},
                                   Natural{1}, 0, target)
                            : integer_bits(value, target);
  }
  auto value = unpack(bits, source);
  if (floating(target)) {
    if (source == TypeKind::kFloat64 && target == TypeKind::kFloat32 &&
        scaled_compare(value.magnitude, Natural{0xffffff}, value.scale - 104) >
            0)
      return std::unexpected(ConstantError::kOutOfRange);
    return pack(value.negative, std::move(value.magnitude), Natural{1},
                value.scale, target);
  }
  const int width = value.magnitude.width() + value.scale;
  if (value.magnitude.zero() || width <= 0) return 0;
  if (width > 64) return std::unexpected(ConstantError::kOutOfRange);
  std::uint64_t magnitude = value.magnitude.low();
  magnitude =
      value.scale >= 0 ? magnitude << value.scale : magnitude >> -value.scale;
  return integer_bits({value.negative, magnitude}, target);
}

ConstantBits convert_integer_mode(std::uint64_t bits, TypeKind source,
                                  TypeKind target, IntegerConversionMode mode) {
  if (!is_valid_integer_bits(bits, source) || !is_integer_type(target)) {
    return std::unexpected(ConstantError::kInvalidOperation);
  }
  const Integer value = integer(bits, source);
  const NumericTypeProperties target_properties =
      *numeric_type_properties(target);
  const std::uint64_t target_mask = mask(target_properties.bit_width);
  if (mode == IntegerConversionMode::kWrap) {
    const std::uint64_t result =
        value.negative ? std::uint64_t{0} - value.magnitude : value.magnitude;
    return result & target_mask;
  }
  if (mode != IntegerConversionMode::kSat) {
    return std::unexpected(ConstantError::kInvalidOperation);
  }

  if (target_properties.category == NumericCategory::kUnsignedInteger) {
    if (value.negative && value.magnitude != 0) {
      return 0;
    }
    return std::min(value.magnitude, target_mask);
  }

  const std::uint64_t minimum_magnitude = std::uint64_t{1}
                                          << (target_properties.bit_width - 1);
  const std::uint64_t maximum = minimum_magnitude - 1;
  if (!value.negative) {
    return std::min(value.magnitude, maximum);
  }
  if (value.magnitude >= minimum_magnitude) {
    return minimum_magnitude;
  }
  return (std::uint64_t{0} - value.magnitude) & target_mask;
}

ConstantBits scalar_signed_literal(LiteralKind literal, std::string_view text,
                                   TypeKind target,
                                   std::span<const TokenKind> signs) {
  auto value =
      scalar_literal(literal, text, target,
                     !signs.empty() && signs.back() == TokenKind::kMinus);
  if (!signs.empty()) signs = signs.first(signs.size() - 1);
  for (auto it = signs.rbegin(); value && it != signs.rend(); ++it)
    value = unary_scalar(*it, target, *value);
  return value;
}

ConstantBits unary_scalar(TokenKind operation, TypeKind type,
                          std::uint64_t bits) {
  if (!is_valid_scalar_bits(type, bits))
    return std::unexpected(ConstantError::kOutOfRange);
  if (operation == TokenKind::kBang && type == TypeKind::kBool) return !bits;
  if (operation == TokenKind::kPlus && is_numeric_type(type)) return bits;
  if (operation == TokenKind::kTilde && is_integer_type(type))
    return ~bits & mask(numeric_type_properties(type)->bit_width);
  if (operation == TokenKind::kMinus && floating(type))
    return bits ^ sign_bit(type);
  if (operation == TokenKind::kMinus && is_integer_type(type)) {
    auto value = integer(bits, type);
    value.negative = !value.negative;
    const auto result = integer_bits(value, type);
    return result ? result
                  : ConstantBits{std::unexpected(ConstantError::kOverflow)};
  }
  return std::unexpected(ConstantError::kInvalidOperation);
}

ConstantBits binary_scalar(TokenKind operation, TypeKind type,
                           std::uint64_t left, std::uint64_t right,
                           TypeKind right_type) {
  if (!is_valid_scalar_bits(type, left) ||
      !is_valid_scalar_bits(right_type, right))
    return std::unexpected(ConstantError::kOutOfRange);
  if (operation == TokenKind::kShiftLeft ||
      operation == TokenKind::kShiftRight) {
    if (!is_integer_type(type) || !is_integer_type(right_type))
      return std::unexpected(ConstantError::kInvalidOperation);
    const auto width = numeric_type_properties(type)->bit_width;
    const auto count = integer(right, right_type);
    if (count.negative || count.magnitude >= width)
      return std::unexpected(ConstantError::kInvalidShift);
    const auto shift = static_cast<unsigned>(count.magnitude);
    if (operation == TokenKind::kShiftLeft)
      return (left << shift) & mask(width);
    std::uint64_t result = left >> shift;
    if (shift && integer(left, type).negative)
      result |= mask(width) ^ mask(width - shift);
    return result;
  }
  if (type != right_type)
    return std::unexpected(ConstantError::kInvalidOperation);
  if (floating(type)) return float_binary(operation, type, left, right);
  if (type == TypeKind::kBool) {
    if (operation == TokenKind::kAmpersandAmpersand) return left && right;
    if (operation == TokenKind::kPipePipe) return left || right;
  }
  if (is_comparison(operation)) {
    int order = left < right ? -1 : left > right ? 1 : 0;
    if (is_integer_type(type) &&
        integer(left, type).negative != integer(right, type).negative)
      order = integer(left, type).negative ? -1 : 1;
    if (!is_numeric_type(type) && operation != TokenKind::kEqualEqual &&
        operation != TokenKind::kBangEqual)
      return std::unexpected(ConstantError::kInvalidOperation);
    return comparison(operation, order);
  }
  if (!is_integer_type(type))
    return std::unexpected(ConstantError::kInvalidOperation);
  if (operation == TokenKind::kAmpersand) return left & right;
  if (operation == TokenKind::kPipe) return left | right;
  if (operation == TokenKind::kCaret) return left ^ right;
  auto a = integer(left, type);
  auto b = integer(right, type);
  if (operation == TokenKind::kMinus) b.negative = !b.negative;
  Integer result{};
  if (operation == TokenKind::kPlus || operation == TokenKind::kMinus) {
    if (a.negative == b.negative) {
      if (a.magnitude > std::numeric_limits<std::uint64_t>::max() - b.magnitude)
        return std::unexpected(ConstantError::kOverflow);
      result = {a.negative, a.magnitude + b.magnitude};
    } else {
      if (a.magnitude < b.magnitude) std::swap(a, b);
      result = {a.negative, a.magnitude - b.magnitude};
    }
  } else if (operation == TokenKind::kStar) {
    if (b.magnitude &&
        a.magnitude > std::numeric_limits<std::uint64_t>::max() / b.magnitude)
      return std::unexpected(ConstantError::kOverflow);
    result = {a.negative != b.negative, a.magnitude * b.magnitude};
  } else if (operation == TokenKind::kSlash ||
             operation == TokenKind::kPercent) {
    if (!b.magnitude) return std::unexpected(ConstantError::kZeroDivisor);
    const auto width = numeric_type_properties(type)->bit_width;
    if (a.negative && a.magnitude == (std::uint64_t{1} << (width - 1)) &&
        b.negative && b.magnitude == 1)
      return std::unexpected(ConstantError::kOverflow);
    result = operation == TokenKind::kSlash
                 ? Integer{a.negative != b.negative, a.magnitude / b.magnitude}
                 : Integer{a.negative, a.magnitude % b.magnitude};
  } else
    return std::unexpected(ConstantError::kInvalidOperation);
  const auto encoded = integer_bits(result, type);
  return encoded ? encoded
                 : ConstantBits{std::unexpected(ConstantError::kOverflow)};
}

bool is_valid_scalar_constant(ScalarConstant value, TypeId expected,
                              const SemanticModel& semantics) {
  if (value.type != expected || expected.value >= semantics.types().size())
    return false;
  const auto& type = semantics.type(expected);
  if (!is_valid_scalar_bits(type.kind, value.bits)) return false;
  if (type.kind != TypeKind::kEnum) return true;
  return type.file && type.file->value < semantics.files().size() &&
         semantics.file(*type.file).kind == FileTypeKind::kEnum &&
         value.bits < semantics.file(*type.file).enum_cases.size();
}

std::string scalar_integer_decimal(TypeKind type, std::uint64_t bits) {
  const auto properties = numeric_type_properties(type);
  if (!properties || !is_valid_integer_bits(bits, type)) return {};
  const bool negative =
      properties->category == NumericCategory::kSignedInteger &&
      (bits & (std::uint64_t{1} << (properties->bit_width - 1))) != 0;
  const std::uint64_t magnitude =
      !negative ? bits
      : properties->bit_width == 64
          ? std::uint64_t{0} - bits
          : (std::uint64_t{1} << properties->bit_width) - bits;
  return std::string{negative ? "-" : ""} + std::to_string(magnitude);
}

}  // namespace cloth
