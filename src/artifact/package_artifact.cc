// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/package_artifact.h"

#include "cloth/abi/aggregate_limits.h"
#include "cloth/identity/package_identity.h"
#include "cloth/lexer/literal.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/visibility.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "sha256.h"

namespace cloth {
namespace {

constexpr std::array<std::uint8_t, 8> kArtifactMagic{0x43, 0x4c, 0x54, 0x48,
                                                     0x50, 0x4b, 0x47, 0x00};
constexpr std::size_t kArtifactHeaderSize = 64;
constexpr std::size_t kDigestOffset = 32;
constexpr std::size_t kDigestSize = 32;

struct JsonValue {
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue, std::less<>>;
  using Data = std::variant<std::nullptr_t, bool, std::string, Array, Object>;

  JsonValue() : data(nullptr) {}
  explicit JsonValue(bool value) : data(value) {}
  explicit JsonValue(std::string value) : data(std::move(value)) {}
  explicit JsonValue(Array value) : data(std::move(value)) {}
  explicit JsonValue(Object value) : data(std::move(value)) {}

  Data data;
};

JsonValue json_string(std::string_view value) {
  return JsonValue{std::string{value}};
}

JsonValue json_integer(std::uint64_t value) {
  return json_string(std::to_string(value));
}

JsonValue json_optional_identity(const std::optional<std::string>& identity);

bool valid_utf8(std::string_view value) {
  return utf8_scalar_count(value).has_value();
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  [[nodiscard]] std::optional<JsonValue> parse() {
    auto result = parse_value(1);
    if (!result || offset_ != input_.size()) {
      if (error_.empty()) error_ = "metadata has trailing bytes";
      return std::nullopt;
    }
    return result;
  }

  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  std::optional<JsonValue> parse_value(std::size_t depth) {
    if (depth > kMaximumArtifactNesting) {
      return fail("metadata nesting exceeds 128");
    }
    if (offset_ == input_.size()) return fail("metadata ends inside a value");
    switch (input_[offset_]) {
      case 'n':
        return parse_null();
      case 't':
      case 'f':
        return parse_bool();
      case '"': {
        auto value = parse_string();
        return value ? std::optional<JsonValue>{json_string(*value)}
                     : std::nullopt;
      }
      case '[':
        return parse_array(depth);
      case '{':
        return parse_object(depth);
      default:
        return fail("metadata contains a forbidden token");
    }
  }

  std::optional<JsonValue> parse_null() {
    if (!consume("null")) return fail("invalid null literal");
    return JsonValue{};
  }

  std::optional<JsonValue> parse_bool() {
    if (consume("true")) return JsonValue{true};
    if (consume("false")) return JsonValue{false};
    return fail("invalid boolean literal");
  }

  std::optional<std::string> parse_string() {
    if (offset_ == input_.size() || input_[offset_] != '"') {
      fail("expected a JSON string");
      return std::nullopt;
    }
    ++offset_;
    std::string result;
    while (offset_ < input_.size() && input_[offset_] != '"') {
      const auto byte = static_cast<unsigned char>(input_[offset_]);
      if (byte < 0x20U) {
        fail("JSON string contains an unescaped control byte");
        return std::nullopt;
      }
      if (input_[offset_] != '\\') {
        result.push_back(input_[offset_++]);
        continue;
      }
      ++offset_;
      if (offset_ == input_.size()) {
        fail("JSON string ends inside an escape");
        return std::nullopt;
      }
      const char escape = input_[offset_++];
      if (escape == '"' || escape == '\\') {
        result.push_back(escape);
        continue;
      }
      if (escape != 'u' || offset_ + 4 > input_.size() ||
          input_[offset_] != '0' || input_[offset_ + 1] != '0') {
        fail("JSON string uses a noncanonical escape");
        return std::nullopt;
      }
      const int upper = hex_value(input_[offset_ + 2]);
      const int lower = hex_value(input_[offset_ + 3]);
      if (upper < 0 || lower < 0 || upper * 16 + lower > 0x1f ||
          !is_lowercase_hex(input_[offset_ + 2]) ||
          !is_lowercase_hex(input_[offset_ + 3])) {
        fail("JSON string has a noncanonical control escape");
        return std::nullopt;
      }
      result.push_back(static_cast<char>(upper * 16 + lower));
      offset_ += 4;
    }
    if (offset_ == input_.size()) {
      fail("JSON string is unterminated");
      return std::nullopt;
    }
    ++offset_;
    if (!valid_utf8(result)) {
      fail("JSON string is not valid UTF-8");
      return std::nullopt;
    }
    return result;
  }

  std::optional<JsonValue> parse_array(std::size_t depth) {
    ++offset_;
    JsonValue::Array result;
    if (consume("]")) return JsonValue{std::move(result)};
    while (true) {
      auto value = parse_value(depth + 1);
      if (!value) return std::nullopt;
      result.push_back(std::move(*value));
      if (consume("]")) return JsonValue{std::move(result)};
      if (!consume(",")) return fail("array elements are not comma-delimited");
    }
  }

  std::optional<JsonValue> parse_object(std::size_t depth) {
    ++offset_;
    JsonValue::Object result;
    std::string previous;
    bool has_previous = false;
    if (consume("}")) return JsonValue{std::move(result)};
    while (true) {
      auto key = parse_string();
      if (!key) return std::nullopt;
      if (has_previous && *key <= previous) {
        return fail("object keys are duplicated or not canonically sorted");
      }
      if (!consume(":")) return fail("object key has no value separator");
      auto value = parse_value(depth + 1);
      if (!value) return std::nullopt;
      previous = *key;
      has_previous = true;
      result.emplace(std::move(*key), std::move(*value));
      if (consume("}")) return JsonValue{std::move(result)};
      if (!consume(",")) return fail("object fields are not comma-delimited");
    }
  }

  std::optional<JsonValue> fail(std::string message) {
    if (error_.empty()) error_ = std::move(message);
    return std::nullopt;
  }

  bool consume(std::string_view value) {
    if (!input_.substr(offset_).starts_with(value)) return false;
    offset_ += value.size();
    return true;
  }

  static int hex_value(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return -1;
  }

  static bool is_lowercase_hex(char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  }

  std::string_view input_;
  std::size_t offset_{0};
  std::string error_;
};

void write_json_string(std::string_view value, std::string& output) {
  constexpr std::string_view kHex = "0123456789abcdef";
  output.push_back('"');
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '"' || character == '\\') {
      output.push_back('\\');
      output.push_back(character);
    } else if (byte < 0x20U) {
      output += "\\u00";
      output.push_back(kHex[byte >> 4U]);
      output.push_back(kHex[byte & 0x0fU]);
    } else {
      output.push_back(character);
    }
  }
  output.push_back('"');
}

void write_json(const JsonValue& value, std::string& output) {
  if (std::holds_alternative<std::nullptr_t>(value.data)) {
    output += "null";
  } else if (const auto* boolean = std::get_if<bool>(&value.data)) {
    output += *boolean ? "true" : "false";
  } else if (const auto* text = std::get_if<std::string>(&value.data)) {
    write_json_string(*text, output);
  } else if (const auto* array = std::get_if<JsonValue::Array>(&value.data)) {
    output.push_back('[');
    for (std::size_t index = 0; index < array->size(); ++index) {
      if (index != 0) output.push_back(',');
      write_json((*array)[index], output);
    }
    output.push_back(']');
  } else {
    const auto& object = std::get<JsonValue::Object>(value.data);
    output.push_back('{');
    std::size_t index = 0;
    for (const auto& [key, field] : object) {
      if (index++ != 0) output.push_back(',');
      write_json_string(key, output);
      output.push_back(':');
      write_json(field, output);
    }
    output.push_back('}');
  }
}

std::string canonical_json(const JsonValue& value) {
  std::string result;
  write_json(value, result);
  return result;
}

std::string binary_hex(std::string_view bytes) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const char character : bytes) {
    const auto byte = static_cast<unsigned char>(character);
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

std::optional<std::string> parse_binary_hex(std::string_view text) {
  if (text.size() % 2 != 0) return std::nullopt;
  std::string result;
  result.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const auto value = [](char character) {
      if (character >= '0' && character <= '9') return character - '0';
      if (character >= 'a' && character <= 'f') return character - 'a' + 10;
      return -1;
    };
    const int upper = value(text[index]);
    const int lower = value(text[index + 1]);
    if (upper < 0 || lower < 0) return std::nullopt;
    result.push_back(static_cast<char>((upper << 4) | lower));
  }
  return result;
}

JsonValue json_identity(std::string_view identity) {
  return json_string(binary_hex(identity));
}

JsonValue json_optional_identity(const std::optional<std::string>& identity) {
  return identity ? json_identity(*identity) : JsonValue{};
}

JsonValue json_string_array(const std::vector<std::string>& values,
                            bool identities = false) {
  JsonValue::Array result;
  result.reserve(values.size());
  for (const std::string& value : values) {
    result.push_back(identities ? json_identity(value) : json_string(value));
  }
  return JsonValue{std::move(result)};
}

std::string_view artifact_kind_name(PackageArtifactKind kind) {
  return kind == PackageArtifactKind::kInterface ? "interface" : "object";
}

std::string_view endianness_name(Endianness value) {
  return value == Endianness::kLittle ? "little" : "big";
}

std::string_view visibility_text(Visibility value) {
  return value == Visibility::kPublic ? "public" : "private";
}

std::string_view file_kind_name(FileTypeKind value) {
  return value == FileTypeKind::kStruct  ? "struct"
         : value == FileTypeKind::kEnum  ? "enum"
         : value == FileTypeKind::kClass ? "class"
                                         : "interface";
}

std::string_view nominal_kind_name(NominalKind value) {
  return value == NominalKind::kStruct  ? "struct"
         : value == NominalKind::kEnum  ? "enum"
         : value == NominalKind::kClass ? "class"
                                        : "interface";
}

std::string_view abi_type_kind_name(AbiTypeKind value) {
  switch (value) {
    case AbiTypeKind::kInvalid:
      return "invalid";
    case AbiTypeKind::kVoid:
      return "void";
    case AbiTypeKind::kInteger:
      return "integer";
    case AbiTypeKind::kFloat:
      return "float";
    case AbiTypeKind::kReference:
      return "reference";
    case AbiTypeKind::kAggregate:
      return "aggregate";
  }
  return "invalid";
}

std::string_view member_kind_name(ImportedMemberKind value) {
  switch (value) {
    case ImportedMemberKind::kField:
      return "field";
    case ImportedMemberKind::kFunction:
      return "function";
    case ImportedMemberKind::kConstructor:
      return "constructor";
  }
  return "function";
}

std::string_view linkage_name(AbiLinkage value) {
  return value == AbiLinkage::kExternal ? "external" : "internal";
}

std::string_view parameter_kind_name(AbiParameterKind value) {
  return value == AbiParameterKind::kResult     ? "result"
         : value == AbiParameterKind::kReceiver ? "receiver"
                                                : "explicit";
}

std::string_view callable_kind_name(AbiCallableKind value) {
  return value == AbiCallableKind::kFunction ? "function" : "constructor";
}

std::string_view symbol_role_name(ArtifactSymbolRole value) {
  return value == ArtifactSymbolRole::kDefinition ? "definition"
                                                  : "requirement";
}

std::string_view symbol_kind_name(ArtifactSymbolKind value) {
  switch (value) {
    case ArtifactSymbolKind::kCallable:
      return "callable";
    case ArtifactSymbolKind::kConstructorInitializer:
      return "constructor_initializer";
    case ArtifactSymbolKind::kStaticField:
      return "static_field";
    case ArtifactSymbolKind::kDescriptor:
      return "descriptor";
    case ArtifactSymbolKind::kRuntime:
      return "runtime";
  }
  return "runtime";
}

JsonValue encode_package(const PackageIdentity& package) {
  return JsonValue{
      JsonValue::Object{{"name", json_string(package.name)},
                        {"version", json_string(package.version)}}};
}

JsonValue encode_location(const ImportedSourceLocation& location) {
  return JsonValue{JsonValue::Object{{"column", json_integer(location.column)},
                                     {"line", json_integer(location.line)},
                                     {"path", json_string(location.path)}}};
}

JsonValue encode_target(const TargetDataLayout& target) {
  return JsonValue{JsonValue::Object{
      {"endianness", json_string(endianness_name(target.endianness))},
      {"float64_alignment", json_integer(target.float64_alignment)},
      {"int64_alignment", json_integer(target.int64_alignment)},
      {"llvm_data_layout", json_string(target.llvm_data_layout)},
      {"object_header_words", json_integer(target.object_header_words)},
      {"pointer_alignment", json_integer(target.pointer.alignment)},
      {"pointer_size", json_integer(target.pointer.size)},
      {"target_name", json_string(target.target_name)}}};
}

JsonValue encode_compatibility(const ArtifactCompatibility& compatibility) {
  JsonValue native;
  if (compatibility.native) {
    JsonValue::Array features;
    for (const std::string& feature : compatibility.native->features) {
      features.push_back(json_string(feature));
    }
    JsonValue::Array tools;
    for (const ArtifactToolIdentity& tool : compatibility.native->tools) {
      tools.push_back(JsonValue{JsonValue::Object{
          {"digest", json_string(artifact_digest_hex(tool.digest))},
          {"name", json_string(tool.name)}}});
    }
    native = JsonValue{JsonValue::Object{
        {"code_model", json_string(compatibility.native->code_model)},
        {"cpu", json_string(compatibility.native->cpu)},
        {"features", JsonValue{std::move(features)}},
        {"object_format", json_string(compatibility.native->object_format)},
        {"relocation_model",
         json_string(compatibility.native->relocation_model)},
        {"runtime_digest", json_string(artifact_digest_hex(
                               compatibility.native->runtime_digest))},
        {"target_triple", json_string(compatibility.native->target_triple)},
        {"tools", JsonValue{std::move(tools)}}}};
  }
  return JsonValue{JsonValue::Object{
      {"compiler_abi", json_integer(compatibility.compiler_abi)},
      {"compiler_id",
       json_string(artifact_digest_hex(compatibility.compiler_id))},
      {"native", std::move(native)},
      {"runtime_abi", json_integer(compatibility.runtime_abi)},
      {"target", encode_target(compatibility.target)}}};
}

JsonValue encode_nominal(const std::optional<NominalIdentity>& nominal) {
  if (!nominal) return JsonValue{};
  return JsonValue{JsonValue::Object{
      {"kind", json_string(nominal_kind_name(nominal->kind))},
      {"name", json_string(nominal->name)},
      {"package", encode_package(nominal->package)},
      {"source_package", json_string(nominal->source_package)}}};
}

JsonValue encode_type(const ImportedType& type) {
  JsonValue::Array references;
  for (const auto offset : type.reference_offsets) {
    references.push_back(json_integer(offset));
  }
  return JsonValue{JsonValue::Object{
      {"abi_kind", json_string(abi_type_kind_name(type.abi_kind))},
      {"bit_width", json_integer(type.bit_width)},
      {"display_name", json_string(type.display_name)},
      {"element", json_optional_identity(type.element_identity)},
      {"id", json_identity(type.identity)},
      {"kind", json_string(type_kind_name(type.kind))},
      {"nominal", encode_nominal(type.nominal_identity)},
      {"reference_offsets", JsonValue{std::move(references)}},
      {"storage", JsonValue{JsonValue::Object{
                      {"alignment", json_integer(type.storage.alignment)},
                      {"size", json_integer(type.storage.size)}}}}}};
}

JsonValue encode_interface_implementation(
    const ImportedInterfaceImplementation& implementation) {
  return JsonValue{JsonValue::Object{
      {"functions",
       json_string_array(implementation.function_identities, true)},
      {"interface", json_identity(implementation.interface_identity)}}};
}

JsonValue encode_file_declaration(const ImportedFile& file) {
  JsonValue::Array cases;
  for (const ImportedEnumCase& item : file.enum_cases) {
    cases.push_back(JsonValue{
        JsonValue::Object{{"id", json_identity(item.identity)},
                          {"location", encode_location(item.location)},
                          {"name", json_string(item.name)},
                          {"tag", json_integer(item.tag)}}});
  }
  JsonValue::Array conformances;
  for (const ImportedInterfaceImplementation& implementation :
       file.interface_implementations) {
    conformances.push_back(encode_interface_implementation(implementation));
  }
  return JsonValue{JsonValue::Object{
      {"abstract", JsonValue{file.is_abstract}},
      {"abstract_functions",
       json_string_array(file.abstract_function_identities, true)},
      {"base", json_optional_identity(file.base_identity)},
      {"conformance", JsonValue{std::move(conformances)}},
      {"direct_interfaces",
       json_string_array(file.direct_interface_identities, true)},
      {"enum_cases", JsonValue{std::move(cases)}},
      {"id", json_identity(file.identity)},
      {"interface_functions",
       json_string_array(file.interface_function_identities, true)},
      {"interface_id",
       file.interface_id ? json_integer(*file.interface_id) : JsonValue{}},
      {"interfaces", json_string_array(file.interface_identities, true)},
      {"kind", json_string(file_kind_name(file.kind))},
      {"location", encode_location(file.location)},
      {"member_order", json_string_array(file.member_order, true)},
      {"name", json_string(file.nominal_identity.name)},
      {"record", json_string("file")},
      {"sealed", JsonValue{file.is_sealed}},
      {"source_package", json_string(file.nominal_identity.source_package)},
      {"virtual_functions",
       json_string_array(file.virtual_function_identities, true)},
      {"visibility", json_string(visibility_text(file.visibility))}}};
}

std::string fixed_hex(std::uint64_t value, std::size_t width) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string result(width, '0');
  for (std::size_t index = 0; index < width; ++index) {
    result[width - index - 1] = kHex[value & 0x0fU];
    value >>= 4U;
  }
  return result;
}

std::optional<JsonValue> encode_literal(const ImportedLiteral& literal,
                                        TypeKind type) {
  if (literal.kind == LiteralKind::kEnum && type == TypeKind::kEnum) {
    return JsonValue{JsonValue::Object{{"kind", json_string("enum")},
                                       {"value", json_string(literal.lexeme)}}};
  }
  if (literal.kind == LiteralKind::kBoolean && type == TypeKind::kBool) {
    if (literal.lexeme == "true") {
      return JsonValue{JsonValue::Object{{"kind", json_string("boolean")},
                                         {"value", JsonValue{true}}}};
    }
    if (literal.lexeme == "false") {
      return JsonValue{JsonValue::Object{{"kind", json_string("boolean")},
                                         {"value", JsonValue{false}}}};
    }
    return std::nullopt;
  }
  if (literal.kind == LiteralKind::kCharacter && type == TypeKind::kChar) {
    if (literal.lexeme.size() < 3) return std::nullopt;
    const char value = literal.lexeme[1] == '\\' && literal.lexeme.size() >= 4
                           ? decode_escape_character(literal.lexeme[2])
                           : literal.lexeme[1];
    return JsonValue{JsonValue::Object{
        {"kind", json_string("character")},
        {"value", json_integer(static_cast<unsigned char>(value))}}};
  }

  const std::optional<NumericTypeProperties> properties =
      numeric_type_properties(type);
  if (!properties) return std::nullopt;
  const char* const begin = literal.lexeme.data();
  const char* const end = begin + literal.lexeme.size();
  if (properties->category == NumericCategory::kFloatingPoint) {
    if (type == TypeKind::kFloat32) {
      float value = 0.0F;
      if (literal.kind == LiteralKind::kInteger) {
        std::uint64_t integer = 0;
        const auto parsed = std::from_chars(begin, end, integer);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
        value = static_cast<float>(integer);
      } else if (literal.kind == LiteralKind::kFloat) {
        const auto parsed =
            std::from_chars(begin, end, value, std::chars_format::general);
        if (parsed.ec != std::errc{} || parsed.ptr != end ||
            !std::isfinite(value)) {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
      return JsonValue{JsonValue::Object{
          {"kind", json_string("float32")},
          {"value",
           json_string(fixed_hex(std::bit_cast<std::uint32_t>(value), 8))}}};
    }
    double value = 0.0;
    if (literal.kind == LiteralKind::kInteger) {
      std::uint64_t integer = 0;
      const auto parsed = std::from_chars(begin, end, integer);
      if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
      value = static_cast<double>(integer);
    } else if (literal.kind == LiteralKind::kFloat) {
      const auto parsed =
          std::from_chars(begin, end, value, std::chars_format::general);
      if (parsed.ec != std::errc{} || parsed.ptr != end ||
          !std::isfinite(value)) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
    return JsonValue{JsonValue::Object{
        {"kind", json_string("float64")},
        {"value",
         json_string(fixed_hex(std::bit_cast<std::uint64_t>(value), 16))}}};
  }

  std::uint64_t value = 0;
  if (literal.kind == LiteralKind::kInteger) {
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
  } else if (literal.kind == LiteralKind::kFloat) {
    double floating = 0.0;
    const auto parsed =
        std::from_chars(begin, end, floating, std::chars_format::general);
    const long double upper = std::ldexp(1.0L, 64);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        !std::isfinite(floating) || floating < 0.0 ||
        static_cast<long double>(std::trunc(floating)) >= upper) {
      return std::nullopt;
    }
    value = static_cast<std::uint64_t>(std::trunc(floating));
  } else {
    return std::nullopt;
  }
  std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  if (properties->category == NumericCategory::kSignedInteger) {
    maximum = (std::uint64_t{1} << (properties->bit_width - 1)) - 1;
  } else if (properties->bit_width < 64) {
    maximum = (std::uint64_t{1} << properties->bit_width) - 1;
  }
  if (value > maximum) return std::nullopt;
  return JsonValue{JsonValue::Object{{"kind", json_string("integer")},
                                     {"value", json_integer(value)}}};
}

JsonValue encode_parameter(const ImportedParameter& parameter) {
  return JsonValue{
      JsonValue::Object{{"final", JsonValue{parameter.is_final}},
                        {"name", json_string(parameter.name)},
                        {"type", json_identity(parameter.type_identity)}}};
}

std::optional<JsonValue> encode_member_declaration(
    const ImportedMember& member,
    const std::map<std::string, TypeKind, std::less<>>& types) {
  JsonValue::Array parameters;
  for (const ImportedParameter& parameter : member.parameters) {
    parameters.push_back(encode_parameter(parameter));
  }
  JsonValue static_value;
  if (member.static_value) {
    const auto type = types.find(member.type_identity);
    if (type == types.end()) return std::nullopt;
    auto encoded = encode_literal(*member.static_value, type->second);
    if (!encoded) return std::nullopt;
    static_value = std::move(*encoded);
  }
  return JsonValue{JsonValue::Object{
      {"abstract", JsonValue{member.is_abstract}},
      {"base_constructor",
       json_optional_identity(member.base_constructor_identity)},
      {"final", JsonValue{member.is_final}},
      {"id", json_identity(member.identity)},
      {"kind", json_string(member_kind_name(member.kind))},
      {"location", encode_location(member.location)},
      {"name", json_string(member.name)},
      {"override", JsonValue{member.is_override}},
      {"overridden", json_optional_identity(member.overridden_identity)},
      {"owner", json_identity(member.owner_identity)},
      {"parameters", JsonValue{std::move(parameters)}},
      {"record", json_string("member")},
      {"slot",
       member.virtual_slot ? json_integer(*member.virtual_slot) : JsonValue{}},
      {"static", JsonValue{member.is_static}},
      {"static_value", std::move(static_value)},
      {"type", json_identity(member.type_identity)},
      {"visibility", json_string(visibility_text(member.visibility))}}};
}

JsonValue encode_field_layout(const ImportedFieldLayout& field) {
  return JsonValue{
      JsonValue::Object{{"field", json_identity(field.field_identity)},
                        {"offset", json_integer(field.offset)},
                        {"type", json_identity(field.type_identity)}}};
}

JsonValue encode_dispatch(const ImportedInterfaceDispatch& dispatch) {
  return JsonValue{JsonValue::Object{
      {"functions", json_string_array(dispatch.function_identities, true)},
      {"id", json_integer(dispatch.interface_id)},
      {"interface", json_identity(dispatch.interface_identity)}}};
}

JsonValue encode_descriptor(const ImportedTypeDescriptor& descriptor) {
  JsonValue::Array interfaces;
  for (const ImportedInterfaceDispatch& dispatch : descriptor.interfaces) {
    interfaces.push_back(encode_dispatch(dispatch));
  }
  JsonValue::Array references;
  for (const std::uint64_t offset : descriptor.reference_offsets) {
    references.push_back(json_integer(offset));
  }
  return JsonValue{JsonValue::Object{
      {"alignment", json_integer(descriptor.alignment)},
      {"display_name", json_string(descriptor.display_name)},
      {"id", json_identity(descriptor.identity)},
      {"interfaces", JsonValue{std::move(interfaces)}},
      {"kind", json_string("file_class")},
      {"mangled_name", json_string(descriptor.mangled_name)},
      {"parent", json_optional_identity(descriptor.parent_identity)},
      {"reference_offsets", JsonValue{std::move(references)}},
      {"size", json_integer(descriptor.size)},
      {"virtual_functions",
       json_string_array(descriptor.virtual_function_identities, true)}}};
}

JsonValue encode_static_field_abi(const ImportedStaticFieldAbi& field) {
  return JsonValue{
      JsonValue::Object{{"linkage", json_string(linkage_name(field.linkage))},
                        {"mangled_name", json_string(field.mangled_name)},
                        {"member", json_identity(field.member_identity)},
                        {"type", json_identity(field.type_identity)}}};
}

JsonValue encode_abi_parameter(const ImportedAbiParameter& parameter) {
  return JsonValue{JsonValue::Object{
      {"kind", json_string(parameter_kind_name(parameter.kind))},
      {"passing", json_string(abi_passing_mode_name(parameter.passing))},
      {"type", json_identity(parameter.type_identity)}}};
}

JsonValue encode_abi_parameters(
    const std::vector<ImportedAbiParameter>& parameters) {
  JsonValue::Array result;
  for (const ImportedAbiParameter& parameter : parameters) {
    result.push_back(encode_abi_parameter(parameter));
  }
  return JsonValue{std::move(result)};
}

JsonValue encode_callable_abi(const ImportedCallableAbi& callable) {
  JsonValue initializer;
  if (callable.initializer_identity) {
    initializer = JsonValue{JsonValue::Object{
        {"id", json_identity(*callable.initializer_identity)},
        {"linkage", json_string(linkage_name(callable.initializer_linkage))},
        {"mangled_name", json_string(callable.initializer_mangled_name)},
        {"parameters", encode_abi_parameters(callable.initializer_parameters)},
        {"receiver_mode", json_string(abi_receiver_mode_name(
                              callable.initializer_receiver_mode))},
        {"return_mode",
         json_string(abi_return_mode_name(callable.initializer_return_mode))},
        {"return_type",
         json_optional_identity(callable.initializer_return_type_identity)}}};
  }
  return JsonValue{JsonValue::Object{
      {"calling_convention", json_string("c")},
      {"initializer", std::move(initializer)},
      {"kind", json_string(callable_kind_name(callable.kind))},
      {"linkage", json_string(linkage_name(callable.linkage))},
      {"mangled_name", json_string(callable.mangled_name)},
      {"member", json_identity(callable.member_identity)},
      {"parameters", encode_abi_parameters(callable.parameters)},
      {"receiver_mode",
       json_string(abi_receiver_mode_name(callable.receiver_mode))},
      {"return_mode", json_string(abi_return_mode_name(callable.return_mode))},
      {"return_type", json_identity(callable.return_type_identity)}}};
}

JsonValue encode_layout(const ImportedFile& file) {
  JsonValue::Array fields;
  for (const ImportedFieldLayout& field : file.abi.fields) {
    fields.push_back(encode_field_layout(field));
  }
  JsonValue::Array static_fields;
  for (const ImportedStaticFieldAbi& field : file.abi.static_fields) {
    static_fields.push_back(encode_static_field_abi(field));
  }
  JsonValue::Array callables;
  for (const ImportedCallableAbi& callable : file.abi.callables) {
    callables.push_back(encode_callable_abi(callable));
  }
  return JsonValue{JsonValue::Object{
      {"alignment", json_integer(file.abi.alignment)},
      {"callables", JsonValue{std::move(callables)}},
      {"descriptor", file.abi.descriptor
                         ? encode_descriptor(*file.abi.descriptor)
                         : JsonValue{}},
      {"fields", JsonValue{std::move(fields)}},
      {"header_size", json_integer(file.abi.header_size)},
      {"owner", json_identity(file.identity)},
      {"size", json_integer(file.abi.size)},
      {"static_fields", JsonValue{std::move(static_fields)}}}};
}

JsonValue encode_source(const ArtifactSource& source) {
  return JsonValue{JsonValue::Object{
      {"digest", json_string(artifact_digest_hex(source.digest))},
      {"path", json_string(source.path)}}};
}

JsonValue encode_dependency(const ArtifactDependency& dependency) {
  return JsonValue{JsonValue::Object{
      {"alias", json_string(dependency.alias)},
      {"digest", json_string(artifact_digest_hex(dependency.digest))},
      {"name", json_string(dependency.package.name)},
      {"version", json_string(dependency.package.version)}}};
}

JsonValue encode_symbol(const ArtifactSymbol& symbol) {
  return JsonValue{JsonValue::Object{
      {"abi_signature", json_string(symbol.abi_signature)},
      {"identity", json_optional_identity(symbol.canonical_identity)},
      {"kind", json_string(symbol_kind_name(symbol.kind))},
      {"link_name", json_string(symbol.link_name)},
      {"role", json_string(symbol_role_name(symbol.role))}}};
}

std::optional<JsonValue> encode_metadata(const PackageArtifact& artifact) {
  JsonValue::Array sources;
  for (const ArtifactSource& source : artifact.sources) {
    sources.push_back(encode_source(source));
  }
  JsonValue::Array dependencies;
  for (const ArtifactDependency& dependency : artifact.dependencies) {
    dependencies.push_back(encode_dependency(dependency));
  }
  JsonValue::Array types;
  std::map<std::string, TypeKind, std::less<>> type_kinds;
  for (const ImportedType& type : artifact.imported.types) {
    types.push_back(encode_type(type));
    type_kinds.emplace(type.identity, type.kind);
  }
  JsonValue::Array declarations;
  struct DeclarationRef {
    std::string_view identity;
    const ImportedFile* file;
    const ImportedMember* member;
  };
  std::vector<DeclarationRef> declaration_refs;
  for (const ImportedFile& file : artifact.imported.files) {
    declaration_refs.push_back({file.identity, &file, nullptr});
    for (const ImportedMember& member : file.members) {
      declaration_refs.push_back({member.identity, nullptr, &member});
    }
  }
  std::ranges::sort(declaration_refs, {}, &DeclarationRef::identity);
  for (const DeclarationRef& declaration : declaration_refs) {
    if (declaration.file != nullptr) {
      declarations.push_back(encode_file_declaration(*declaration.file));
      continue;
    }
    auto member = encode_member_declaration(*declaration.member, type_kinds);
    if (!member) return std::nullopt;
    declarations.push_back(std::move(*member));
  }
  JsonValue::Array layouts;
  for (const ImportedFile& file : artifact.imported.files) {
    layouts.push_back(encode_layout(file));
  }
  JsonValue::Array symbols;
  for (const ArtifactSymbol& symbol : artifact.symbols) {
    symbols.push_back(encode_symbol(symbol));
  }
  return JsonValue{JsonValue::Object{
      {"compatibility", encode_compatibility(artifact.compatibility)},
      {"declarations", JsonValue{std::move(declarations)}},
      {"dependencies", JsonValue{std::move(dependencies)}},
      {"kind", json_string(artifact_kind_name(artifact.kind))},
      {"layouts", JsonValue{std::move(layouts)}},
      {"package", encode_package(artifact.imported.package)},
      {"sources", JsonValue{std::move(sources)}},
      {"symbols", JsonValue{std::move(symbols)}},
      {"types", JsonValue{std::move(types)}}}};
}

class MetadataDecoder {
 public:
  explicit MetadataDecoder(const JsonValue& root) : root_(root) {}

  [[nodiscard]] std::optional<PackageArtifact> decode();

  [[nodiscard]] std::vector<ArtifactIssue> take_issues() {
    return std::move(issues_);
  }

 private:
  void issue(std::string record, std::string message) {
    if (issues_.size() < 32) {
      issues_.push_back(ArtifactIssue{ArtifactIssueCode::kMalformedMetadata,
                                      std::move(record), std::move(message)});
    }
  }

  const JsonValue::Object* object(
      const JsonValue& value, std::initializer_list<std::string_view> fields,
      std::string_view record) {
    const auto* result = std::get_if<JsonValue::Object>(&value.data);
    if (result == nullptr || result->size() != fields.size() ||
        std::ranges::any_of(fields, [&](std::string_view field) {
          return !result->contains(field);
        })) {
      issue(std::string{record}, "object has missing or unknown fields");
      return nullptr;
    }
    return result;
  }

  const JsonValue::Array* array(const JsonValue& value,
                                std::string_view record) {
    const auto* result = std::get_if<JsonValue::Array>(&value.data);
    if (result == nullptr) issue(std::string{record}, "expected an array");
    return result;
  }

  const std::string* text(const JsonValue& value, std::string_view record) {
    const auto* result = std::get_if<std::string>(&value.data);
    if (result == nullptr) issue(std::string{record}, "expected a string");
    return result;
  }

  std::optional<bool> boolean(const JsonValue& value, std::string_view record) {
    const auto* result = std::get_if<bool>(&value.data);
    if (result == nullptr) {
      issue(std::string{record}, "expected a boolean");
      return std::nullopt;
    }
    return *result;
  }

  bool is_null(const JsonValue& value) const {
    return std::holds_alternative<std::nullptr_t>(value.data);
  }

  std::optional<std::uint64_t> integer(const JsonValue& value,
                                       std::string_view record) {
    const std::string* value_text = text(value, record);
    if (value_text == nullptr || value_text->empty() ||
        (value_text->size() > 1 && value_text->front() == '0')) {
      if (value_text != nullptr) {
        issue(std::string{record}, "integer string is not canonical");
      }
      return std::nullopt;
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        value_text->data(), value_text->data() + value_text->size(), result);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value_text->data() + value_text->size()) {
      issue(std::string{record}, "integer string is out of range");
      return std::nullopt;
    }
    return result;
  }

  std::optional<std::string> identity(const JsonValue& value,
                                      std::string_view record) {
    const std::string* encoded = text(value, record);
    if (encoded == nullptr) return std::nullopt;
    auto result = parse_binary_hex(*encoded);
    if (!result) issue(std::string{record}, "identity is not lowercase hex");
    return result;
  }

  std::optional<std::optional<std::string>> optional_identity(
      const JsonValue& value, std::string_view record) {
    if (is_null(value)) return std::optional<std::string>{};
    auto result = identity(value, record);
    if (!result) return std::nullopt;
    return std::optional<std::string>{std::move(*result)};
  }

  std::optional<ArtifactDigest> digest(const JsonValue& value,
                                       std::string_view record) {
    const std::string* encoded = text(value, record);
    if (encoded == nullptr) return std::nullopt;
    auto result = parse_artifact_digest(*encoded);
    if (!result) issue(std::string{record}, "digest is not lowercase SHA-256");
    return result;
  }

  std::optional<std::vector<std::string>> identity_array(
      const JsonValue& value, std::string_view record) {
    const JsonValue::Array* values = array(value, record);
    if (values == nullptr) return std::nullopt;
    std::vector<std::string> result;
    result.reserve(values->size());
    for (const JsonValue& item : *values) {
      auto decoded = identity(item, record);
      if (!decoded) return std::nullopt;
      result.push_back(std::move(*decoded));
    }
    return result;
  }

  std::optional<PackageIdentity> decode_package(const JsonValue& value,
                                                std::string_view record);
  std::optional<ImportedSourceLocation> decode_location(
      const JsonValue& value, std::string_view record);
  std::optional<TargetDataLayout> decode_target(const JsonValue& value);
  std::optional<ArtifactCompatibility> decode_compatibility(
      const JsonValue& value);
  std::optional<std::vector<ArtifactSource>> decode_sources(
      const JsonValue& value);
  std::optional<std::vector<ArtifactDependency>> decode_dependencies(
      const JsonValue& value);
  std::optional<std::vector<ImportedType>> decode_types(const JsonValue& value);
  std::optional<ImportedLiteral> decode_literal(const JsonValue& value,
                                                TypeKind type);
  std::optional<ImportedParameter> decode_parameter(const JsonValue& value);
  std::optional<ImportedFile> decode_file_declaration(
      const JsonValue& value, const PackageIdentity& package);
  std::optional<ImportedMember> decode_member_declaration(
      const JsonValue& value,
      const std::map<std::string, TypeKind, std::less<>>& type_kinds);
  bool decode_declarations(
      const JsonValue& value, const PackageIdentity& package,
      const std::map<std::string, TypeKind, std::less<>>& type_kinds,
      std::vector<ImportedFile>& files);
  std::optional<ImportedAbiParameter> decode_abi_parameter(
      const JsonValue& value);
  std::optional<ImportedCallableAbi> decode_callable(const JsonValue& value);
  std::optional<ImportedTypeDescriptor> decode_descriptor(
      const JsonValue& value);
  std::optional<ImportedClassAbi> decode_layout(const JsonValue& value,
                                                std::string& owner);
  bool decode_layouts(const JsonValue& value, std::vector<ImportedFile>& files);
  std::optional<std::vector<ArtifactSymbol>> decode_symbols(
      const JsonValue& value);

  const JsonValue& root_;
  std::vector<ArtifactIssue> issues_;
};

std::optional<PackageIdentity> MetadataDecoder::decode_package(
    const JsonValue& value, std::string_view record) {
  const auto* fields = object(value, {"name", "version"}, record);
  if (fields == nullptr) return std::nullopt;
  const std::string* name = text(fields->at("name"), record);
  const std::string* version = text(fields->at("version"), record);
  if (name == nullptr || version == nullptr) return std::nullopt;
  return PackageIdentity{*name, *version};
}

std::optional<ImportedSourceLocation> MetadataDecoder::decode_location(
    const JsonValue& value, std::string_view record) {
  const auto* fields = object(value, {"column", "line", "path"}, record);
  if (fields == nullptr) return std::nullopt;
  const auto column = integer(fields->at("column"), record);
  const auto line = integer(fields->at("line"), record);
  const std::string* path = text(fields->at("path"), record);
  if (!column || !line || path == nullptr) return std::nullopt;
  return ImportedSourceLocation{*path, *line, *column};
}

std::optional<TargetDataLayout> MetadataDecoder::decode_target(
    const JsonValue& value) {
  constexpr std::string_view kRecord = "compatibility.target";
  const auto* fields =
      object(value,
             {"endianness", "float64_alignment", "int64_alignment",
              "llvm_data_layout", "object_header_words", "pointer_alignment",
              "pointer_size", "target_name"},
             kRecord);
  if (fields == nullptr) return std::nullopt;
  const std::string* endian = text(fields->at("endianness"), kRecord);
  const auto float64_alignment =
      integer(fields->at("float64_alignment"), kRecord);
  const auto int64_alignment = integer(fields->at("int64_alignment"), kRecord);
  const std::string* llvm = text(fields->at("llvm_data_layout"), kRecord);
  const auto header = integer(fields->at("object_header_words"), kRecord);
  const auto pointer_alignment =
      integer(fields->at("pointer_alignment"), kRecord);
  const auto pointer_size = integer(fields->at("pointer_size"), kRecord);
  const std::string* target_name = text(fields->at("target_name"), kRecord);
  if (endian == nullptr || !float64_alignment || !int64_alignment ||
      llvm == nullptr || !header || !pointer_alignment || !pointer_size ||
      target_name == nullptr || (*endian != "little" && *endian != "big")) {
    if (endian != nullptr && *endian != "little" && *endian != "big") {
      issue(std::string{kRecord}, "unknown endianness");
    }
    return std::nullopt;
  }
  return TargetDataLayout{
      *target_name,
      *llvm,
      *endian == "little" ? Endianness::kLittle : Endianness::kBig,
      SizeAlignment{*pointer_size, *pointer_alignment},
      *int64_alignment,
      *float64_alignment,
      *header};
}

std::optional<ArtifactCompatibility> MetadataDecoder::decode_compatibility(
    const JsonValue& value) {
  constexpr std::string_view kRecord = "compatibility";
  const auto* fields = object(
      value, {"compiler_abi", "compiler_id", "native", "runtime_abi", "target"},
      kRecord);
  if (fields == nullptr) return std::nullopt;
  const auto compiler_abi = integer(fields->at("compiler_abi"), kRecord);
  const auto compiler_id = digest(fields->at("compiler_id"), kRecord);
  const auto runtime_abi = integer(fields->at("runtime_abi"), kRecord);
  auto target = decode_target(fields->at("target"));
  if (!compiler_abi ||
      *compiler_abi > std::numeric_limits<std::uint32_t>::max() ||
      !compiler_id || !runtime_abi ||
      *runtime_abi > std::numeric_limits<std::uint32_t>::max() || !target) {
    return std::nullopt;
  }

  std::optional<ArtifactNativeCompatibility> native;
  if (!is_null(fields->at("native"))) {
    const auto* native_fields =
        object(fields->at("native"),
               {"code_model", "cpu", "features", "object_format",
                "relocation_model", "runtime_digest", "target_triple", "tools"},
               "compatibility.native");
    if (native_fields == nullptr) return std::nullopt;
    const std::string* code_model =
        text(native_fields->at("code_model"), "compatibility.native");
    const std::string* cpu =
        text(native_fields->at("cpu"), "compatibility.native");
    const std::string* object_format =
        text(native_fields->at("object_format"), "compatibility.native");
    const std::string* relocation =
        text(native_fields->at("relocation_model"), "compatibility.native");
    const auto runtime =
        digest(native_fields->at("runtime_digest"), "compatibility.native");
    const std::string* triple =
        text(native_fields->at("target_triple"), "compatibility.native");
    const JsonValue::Array* feature_values =
        array(native_fields->at("features"), "compatibility.native.features");
    const JsonValue::Array* tool_values =
        array(native_fields->at("tools"), "compatibility.native.tools");
    if (code_model == nullptr || cpu == nullptr || object_format == nullptr ||
        relocation == nullptr || !runtime || triple == nullptr ||
        feature_values == nullptr || tool_values == nullptr) {
      return std::nullopt;
    }
    std::vector<std::string> features;
    for (const JsonValue& item : *feature_values) {
      const std::string* feature = text(item, "compatibility.native.features");
      if (feature == nullptr) return std::nullopt;
      features.push_back(*feature);
    }
    std::vector<ArtifactToolIdentity> tools;
    for (const JsonValue& item : *tool_values) {
      const auto* tool =
          object(item, {"digest", "name"}, "compatibility.native.tools");
      if (tool == nullptr) return std::nullopt;
      auto tool_digest =
          digest(tool->at("digest"), "compatibility.native.tools");
      const std::string* name =
          text(tool->at("name"), "compatibility.native.tools");
      if (!tool_digest || name == nullptr) return std::nullopt;
      tools.push_back(ArtifactToolIdentity{*name, *tool_digest});
    }
    native = ArtifactNativeCompatibility{
        *triple,     *object_format, *cpu,     std::move(features),
        *relocation, *code_model,    *runtime, std::move(tools)};
  }
  return ArtifactCompatibility{static_cast<std::uint32_t>(*compiler_abi),
                               static_cast<std::uint32_t>(*runtime_abi),
                               *compiler_id, std::move(*target),
                               std::move(native)};
}

std::optional<std::vector<ArtifactSource>> MetadataDecoder::decode_sources(
    const JsonValue& value) {
  const auto* values = array(value, "sources");
  if (values == nullptr) return std::nullopt;
  std::vector<ArtifactSource> result;
  for (const JsonValue& item : *values) {
    const auto* fields = object(item, {"digest", "path"}, "sources");
    if (fields == nullptr) return std::nullopt;
    auto source_digest = digest(fields->at("digest"), "sources");
    const std::string* path = text(fields->at("path"), "sources");
    if (!source_digest || path == nullptr) return std::nullopt;
    result.push_back(ArtifactSource{*path, *source_digest});
  }
  return result;
}

std::optional<std::vector<ArtifactDependency>>
MetadataDecoder::decode_dependencies(const JsonValue& value) {
  const auto* values = array(value, "dependencies");
  if (values == nullptr) return std::nullopt;
  std::vector<ArtifactDependency> result;
  for (const JsonValue& item : *values) {
    const auto* fields =
        object(item, {"alias", "digest", "name", "version"}, "dependencies");
    if (fields == nullptr) return std::nullopt;
    const std::string* alias = text(fields->at("alias"), "dependencies");
    auto dependency_digest = digest(fields->at("digest"), "dependencies");
    const std::string* name = text(fields->at("name"), "dependencies");
    const std::string* version = text(fields->at("version"), "dependencies");
    if (alias == nullptr || !dependency_digest || name == nullptr ||
        version == nullptr) {
      return std::nullopt;
    }
    result.push_back(ArtifactDependency{
        *alias, PackageIdentity{*name, *version}, *dependency_digest});
  }
  return result;
}

std::optional<TypeKind> parse_type_kind(std::string_view value) {
  constexpr std::array kinds{
      TypeKind::kError,     TypeKind::kVoid,      TypeKind::kNull,
      TypeKind::kBool,      TypeKind::kChar,      TypeKind::kByte,
      TypeKind::kInt8,      TypeKind::kInt16,     TypeKind::kInt32,
      TypeKind::kInt64,     TypeKind::kUint8,     TypeKind::kUint16,
      TypeKind::kUint32,    TypeKind::kUint64,    TypeKind::kFloat32,
      TypeKind::kFloat64,   TypeKind::kString,    TypeKind::kObject,
      TypeKind::kFileClass, TypeKind::kInterface, TypeKind::kEnum,
      TypeKind::kArray,     TypeKind::kNullable,  TypeKind::kStruct};
  const auto kind = std::ranges::find_if(kinds, [&](TypeKind candidate) {
    return type_kind_name(candidate) == value;
  });
  return kind == kinds.end() ? std::nullopt : std::optional<TypeKind>{*kind};
}

std::optional<AbiTypeKind> parse_abi_type_kind(std::string_view value) {
  if (value == "invalid") return AbiTypeKind::kInvalid;
  if (value == "void") return AbiTypeKind::kVoid;
  if (value == "integer") return AbiTypeKind::kInteger;
  if (value == "float") return AbiTypeKind::kFloat;
  if (value == "reference") return AbiTypeKind::kReference;
  if (value == "aggregate") return AbiTypeKind::kAggregate;
  return std::nullopt;
}

std::optional<std::vector<ImportedType>> MetadataDecoder::decode_types(
    const JsonValue& value) {
  const auto* values = array(value, "types");
  if (values == nullptr) return std::nullopt;
  std::vector<ImportedType> result;
  for (const JsonValue& item : *values) {
    const auto* fields =
        object(item,
               {"abi_kind", "bit_width", "display_name", "element", "id",
                "kind", "nominal", "reference_offsets", "storage"},
               "types");
    if (fields == nullptr) return std::nullopt;
    const std::string* abi_kind_text = text(fields->at("abi_kind"), "types");
    const auto bits = integer(fields->at("bit_width"), "types");
    const std::string* display = text(fields->at("display_name"), "types");
    auto element = optional_identity(fields->at("element"), "types");
    auto id = identity(fields->at("id"), "types");
    const std::string* kind_text = text(fields->at("kind"), "types");
    const auto* storage =
        object(fields->at("storage"), {"alignment", "size"}, "types.storage");
    if (abi_kind_text == nullptr || !bits ||
        *bits > std::numeric_limits<std::uint32_t>::max() ||
        display == nullptr || !element || !id || kind_text == nullptr ||
        storage == nullptr) {
      return std::nullopt;
    }
    const auto abi_kind = parse_abi_type_kind(*abi_kind_text);
    const auto kind = parse_type_kind(*kind_text);
    const auto alignment = integer(storage->at("alignment"), "types.storage");
    const auto size = integer(storage->at("size"), "types.storage");
    if (!abi_kind || !kind || !alignment || !size) {
      issue("types", "type enum or storage value is invalid");
      return std::nullopt;
    }
    std::optional<NominalIdentity> nominal;
    if (!is_null(fields->at("nominal"))) {
      const auto* nominal_fields = object(
          fields->at("nominal"), {"kind", "name", "package", "source_package"},
          "types.nominal");
      if (nominal_fields == nullptr) return std::nullopt;
      const std::string* nominal_kind =
          text(nominal_fields->at("kind"), "types.nominal");
      const std::string* name =
          text(nominal_fields->at("name"), "types.nominal");
      auto package =
          decode_package(nominal_fields->at("package"), "types.nominal");
      const std::string* source_package =
          text(nominal_fields->at("source_package"), "types.nominal");
      if (nominal_kind == nullptr || name == nullptr || !package ||
          source_package == nullptr ||
          (*nominal_kind != "class" && *nominal_kind != "interface" &&
           *nominal_kind != "enum" && *nominal_kind != "struct")) {
        issue("types.nominal", "nominal type enum is invalid");
        return std::nullopt;
      }
      nominal =
          NominalIdentity{std::move(*package), *source_package, *name,
                          *nominal_kind == "struct"  ? NominalKind::kStruct
                          : *nominal_kind == "enum"  ? NominalKind::kEnum
                          : *nominal_kind == "class" ? NominalKind::kClass
                                                     : NominalKind::kInterface};
    }
    const auto* reference_values =
        array(fields->at("reference_offsets"), "types");
    if (reference_values == nullptr ||
        reference_values->size() > kMaxLayoutReferences) {
      issue("types", "invalid or excessive reference map");
      return std::nullopt;
    }
    std::vector<std::uint64_t> references;
    for (const JsonValue& reference : *reference_values) {
      const auto offset = integer(reference, "types.reference_offsets");
      if (!offset) return std::nullopt;
      references.push_back(*offset);
    }
    result.push_back(ImportedType{
        std::move(*id), *kind, *display, std::move(*element),
        std::move(nominal), *abi_kind, static_cast<std::uint32_t>(*bits),
        SizeAlignment{*size, *alignment}, std::move(references)});
  }
  return result;
}

std::optional<Visibility> parse_visibility(std::string_view value) {
  if (value == "public") return Visibility::kPublic;
  if (value == "private") return Visibility::kPrivate;
  return std::nullopt;
}

std::optional<FileTypeKind> parse_file_kind(std::string_view value) {
  if (value == "class") return FileTypeKind::kClass;
  if (value == "interface") return FileTypeKind::kInterface;
  if (value == "enum") return FileTypeKind::kEnum;
  if (value == "struct") return FileTypeKind::kStruct;
  return std::nullopt;
}

std::optional<ImportedMemberKind> parse_member_kind(std::string_view value) {
  if (value == "field") return ImportedMemberKind::kField;
  if (value == "function") return ImportedMemberKind::kFunction;
  if (value == "constructor") return ImportedMemberKind::kConstructor;
  return std::nullopt;
}

std::optional<std::uint64_t> parse_fixed_hex(std::string_view value,
                                             std::size_t width) {
  if (value.size() != width) return std::nullopt;
  std::uint64_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      std::ranges::any_of(value, [](char character) {
        return !((character >= '0' && character <= '9') ||
                 (character >= 'a' && character <= 'f'));
      })) {
    return std::nullopt;
  }
  return result;
}

template <typename Value>
std::optional<std::string> float_lexeme(Value value) {
  std::array<char, 128> output{};
  const auto result = std::to_chars(
      output.data(), output.data() + output.size(), value,
      std::chars_format::scientific, std::numeric_limits<Value>::max_digits10);
  if (result.ec != std::errc{}) return std::nullopt;
  return std::string{output.data(), result.ptr};
}

std::string character_lexeme(std::uint64_t value) {
  const char character = static_cast<char>(value);
  std::string result{"'"};
  switch (character) {
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    case '\0':
      result += "\\0";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\'':
      result += "\\'";
      break;
    default:
      result.push_back(character);
      break;
  }
  result.push_back('\'');
  return result;
}

std::optional<ImportedLiteral> MetadataDecoder::decode_literal(
    const JsonValue& value, TypeKind type) {
  const auto* fields = object(value, {"kind", "value"}, "static_value");
  if (fields == nullptr) return std::nullopt;
  const std::string* kind = text(fields->at("kind"), "static_value");
  if (kind == nullptr) return std::nullopt;
  if (*kind == "enum" && type == TypeKind::kEnum) {
    const auto tag = integer(fields->at("value"), "enum constant");
    if (!tag || *tag >= kMaxEnumCases) {
      issue("static_value", "enum tag is out of range");
      return std::nullopt;
    }
    return ImportedLiteral{LiteralKind::kEnum, std::to_string(*tag)};
  }
  if (*kind == "boolean" && type == TypeKind::kBool) {
    const auto decoded = boolean(fields->at("value"), "static_value");
    if (!decoded) return std::nullopt;
    return ImportedLiteral{LiteralKind::kBoolean, *decoded ? "true" : "false"};
  }
  if (*kind == "character" && type == TypeKind::kChar) {
    const auto decoded = integer(fields->at("value"), "static_value");
    if (!decoded || *decoded > 255) {
      issue("static_value", "character value is out of range");
      return std::nullopt;
    }
    return ImportedLiteral{LiteralKind::kCharacter, character_lexeme(*decoded)};
  }
  const auto properties = numeric_type_properties(type);
  if (*kind == "integer" && properties &&
      properties->category != NumericCategory::kFloatingPoint) {
    const auto decoded = integer(fields->at("value"), "static_value");
    if (!decoded) return std::nullopt;
    std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (properties->category == NumericCategory::kSignedInteger) {
      maximum = (std::uint64_t{1} << (properties->bit_width - 1)) - 1;
    } else if (properties->bit_width < 64) {
      maximum = (std::uint64_t{1} << properties->bit_width) - 1;
    }
    if (*decoded > maximum) {
      issue("static_value", "integer value is out of range for its type");
      return std::nullopt;
    }
    return ImportedLiteral{LiteralKind::kInteger, std::to_string(*decoded)};
  }
  const std::string* encoded = text(fields->at("value"), "static_value");
  if (encoded == nullptr) return std::nullopt;
  if (*kind == "float32" && type == TypeKind::kFloat32) {
    const auto bits = parse_fixed_hex(*encoded, 8);
    if (!bits) {
      issue("static_value", "float32 bits are not canonical");
      return std::nullopt;
    }
    const float decoded =
        std::bit_cast<float>(static_cast<std::uint32_t>(*bits));
    auto lexeme = float_lexeme(decoded);
    if (!std::isfinite(decoded) || !lexeme) {
      issue("static_value", "float32 constant is not finite");
      return std::nullopt;
    }
    return ImportedLiteral{LiteralKind::kFloat, std::move(*lexeme)};
  }
  if (*kind == "float64" && type == TypeKind::kFloat64) {
    const auto bits = parse_fixed_hex(*encoded, 16);
    if (!bits) {
      issue("static_value", "float64 bits are not canonical");
      return std::nullopt;
    }
    const double decoded = std::bit_cast<double>(*bits);
    auto lexeme = float_lexeme(decoded);
    if (!std::isfinite(decoded) || !lexeme) {
      issue("static_value", "float64 constant is not finite");
      return std::nullopt;
    }
    return ImportedLiteral{LiteralKind::kFloat, std::move(*lexeme)};
  }
  issue("static_value", "literal kind does not match its declared type");
  return std::nullopt;
}

std::optional<ImportedParameter> MetadataDecoder::decode_parameter(
    const JsonValue& value) {
  const auto* fields = object(value, {"final", "name", "type"}, "parameter");
  if (fields == nullptr) return std::nullopt;
  const auto final = boolean(fields->at("final"), "parameter");
  const std::string* name = text(fields->at("name"), "parameter");
  auto type = identity(fields->at("type"), "parameter");
  if (!final || name == nullptr || !type) return std::nullopt;
  return ImportedParameter{*name, std::move(*type), *final};
}

std::optional<ImportedFile> MetadataDecoder::decode_file_declaration(
    const JsonValue& value, const PackageIdentity& package) {
  constexpr std::string_view kRecord = "declarations.file";
  const auto* fields = object(
      value,
      {"abstract", "abstract_functions", "base", "conformance",
       "direct_interfaces", "enum_cases", "id", "interface_functions",
       "interface_id", "interfaces", "kind", "location", "member_order", "name",
       "record", "sealed", "source_package", "virtual_functions", "visibility"},
      kRecord);
  if (fields == nullptr) return std::nullopt;
  const auto abstract = boolean(fields->at("abstract"), kRecord);
  auto abstract_functions =
      identity_array(fields->at("abstract_functions"), kRecord);
  auto base = optional_identity(fields->at("base"), kRecord);
  auto direct_interfaces =
      identity_array(fields->at("direct_interfaces"), kRecord);
  auto id = identity(fields->at("id"), kRecord);
  auto interface_functions =
      identity_array(fields->at("interface_functions"), kRecord);
  std::optional<std::uint64_t> interface_id;
  if (!is_null(fields->at("interface_id"))) {
    interface_id = integer(fields->at("interface_id"), kRecord);
    if (!interface_id) return std::nullopt;
  }
  auto interfaces = identity_array(fields->at("interfaces"), kRecord);
  const std::string* kind_text = text(fields->at("kind"), kRecord);
  auto location = decode_location(fields->at("location"), kRecord);
  auto member_order = identity_array(fields->at("member_order"), kRecord);
  const std::string* name = text(fields->at("name"), kRecord);
  const std::string* record = text(fields->at("record"), kRecord);
  const auto sealed = boolean(fields->at("sealed"), kRecord);
  const std::string* source_package =
      text(fields->at("source_package"), kRecord);
  auto virtual_functions =
      identity_array(fields->at("virtual_functions"), kRecord);
  const std::string* visibility_value = text(fields->at("visibility"), kRecord);
  const auto kind =
      kind_text == nullptr ? std::nullopt : parse_file_kind(*kind_text);
  const auto visibility = visibility_value == nullptr
                              ? std::nullopt
                              : parse_visibility(*visibility_value);
  if (!abstract || !abstract_functions || !base || !direct_interfaces || !id ||
      !interface_functions || !interfaces || !kind || !location ||
      !member_order || name == nullptr || record == nullptr ||
      *record != "file" || !sealed || source_package == nullptr ||
      !virtual_functions || !visibility) {
    issue(std::string{kRecord}, "file declaration value is invalid");
    return std::nullopt;
  }

  const JsonValue::Array* conformance_values =
      array(fields->at("conformance"), kRecord);
  if (conformance_values == nullptr) return std::nullopt;
  std::vector<ImportedInterfaceImplementation> conformances;
  for (const JsonValue& item : *conformance_values) {
    const auto* implementation =
        object(item, {"functions", "interface"}, kRecord);
    if (implementation == nullptr) return std::nullopt;
    auto functions = identity_array(implementation->at("functions"), kRecord);
    auto interface_identity =
        identity(implementation->at("interface"), kRecord);
    if (!functions || !interface_identity) return std::nullopt;
    conformances.push_back(ImportedInterfaceImplementation{
        std::move(*interface_identity), std::move(*functions)});
  }

  const JsonValue::Array* case_values =
      array(fields->at("enum_cases"), kRecord);
  if (case_values == nullptr || case_values->size() > kMaxEnumCases) {
    issue(std::string{kRecord},
          "invalid enum case list or case limit exceeded");
    return std::nullopt;
  }
  std::vector<ImportedEnumCase> enum_cases;
  for (const JsonValue& item : *case_values) {
    const auto* entry =
        object(item, {"id", "location", "name", "tag"}, kRecord);
    if (entry == nullptr) return std::nullopt;
    auto case_id = identity(entry->at("id"), kRecord);
    auto case_location = decode_location(entry->at("location"), kRecord);
    const auto* case_name = text(entry->at("name"), kRecord);
    auto tag = integer(entry->at("tag"), kRecord);
    if (!case_id || !case_location || case_name == nullptr || !tag ||
        *tag >= kMaxEnumCases) {
      issue(std::string{kRecord}, "invalid enum case record");
      return std::nullopt;
    }
    enum_cases.push_back(ImportedEnumCase{std::move(*case_id), *case_name,
                                          static_cast<std::uint32_t>(*tag),
                                          std::move(*case_location)});
  }
  const NominalKind nominal_kind =
      *kind == FileTypeKind::kStruct  ? NominalKind::kStruct
      : *kind == FileTypeKind::kEnum  ? NominalKind::kEnum
      : *kind == FileTypeKind::kClass ? NominalKind::kClass
                                      : NominalKind::kInterface;
  const NominalIdentity nominal{package, *source_package, *name, nominal_kind};
  ImportedTypeDescriptor descriptor{AbiHeapObjectKind::kFileClass,
                                    {},
                                    std::nullopt,
                                    {},
                                    0,
                                    1,
                                    {},
                                    {},
                                    {},
                                    {}};
  return ImportedFile{
      nominal,
      std::move(*id),
      location->path,
      std::move(*location),
      *visibility,
      *kind,
      *abstract,
      *sealed,
      std::move(*base),
      std::move(*direct_interfaces),
      std::move(*interfaces),
      interface_id,
      {},
      std::move(*member_order),
      std::move(*virtual_functions),
      std::move(*abstract_functions),
      std::move(*interface_functions),
      std::move(conformances),
      ImportedClassAbi{0, 0, 1, {}, std::move(descriptor), {}, {}},
      std::move(enum_cases)};
}

std::optional<ImportedMember> MetadataDecoder::decode_member_declaration(
    const JsonValue& value,
    const std::map<std::string, TypeKind, std::less<>>& type_kinds) {
  constexpr std::string_view kRecord = "declarations.member";
  const auto* fields =
      object(value,
             {"abstract", "base_constructor", "final", "id", "kind", "location",
              "name", "override", "overridden", "owner", "parameters", "record",
              "slot", "static", "static_value", "type", "visibility"},
             kRecord);
  if (fields == nullptr) return std::nullopt;
  const auto abstract = boolean(fields->at("abstract"), kRecord);
  auto base = optional_identity(fields->at("base_constructor"), kRecord);
  const auto final = boolean(fields->at("final"), kRecord);
  auto id = identity(fields->at("id"), kRecord);
  const std::string* kind_text = text(fields->at("kind"), kRecord);
  auto location = decode_location(fields->at("location"), kRecord);
  const std::string* name = text(fields->at("name"), kRecord);
  const auto override = boolean(fields->at("override"), kRecord);
  auto overridden = optional_identity(fields->at("overridden"), kRecord);
  auto owner = identity(fields->at("owner"), kRecord);
  const std::string* record = text(fields->at("record"), kRecord);
  std::optional<std::uint64_t> slot;
  if (!is_null(fields->at("slot"))) {
    slot = integer(fields->at("slot"), kRecord);
    if (!slot) return std::nullopt;
  }
  const auto is_static = boolean(fields->at("static"), kRecord);
  auto type = identity(fields->at("type"), kRecord);
  const std::string* visibility_value = text(fields->at("visibility"), kRecord);
  const auto kind =
      kind_text == nullptr ? std::nullopt : parse_member_kind(*kind_text);
  const auto visibility = visibility_value == nullptr
                              ? std::nullopt
                              : parse_visibility(*visibility_value);
  if (!abstract || !base || !final || !id || !kind || !location ||
      name == nullptr || !override || !overridden || !owner ||
      record == nullptr || *record != "member" || !is_static || !type ||
      !visibility) {
    issue(std::string{kRecord}, "member declaration value is invalid");
    return std::nullopt;
  }
  const JsonValue::Array* parameter_values =
      array(fields->at("parameters"), kRecord);
  if (parameter_values == nullptr) return std::nullopt;
  std::vector<ImportedParameter> parameters;
  for (const JsonValue& item : *parameter_values) {
    auto parameter = decode_parameter(item);
    if (!parameter) return std::nullopt;
    parameters.push_back(std::move(*parameter));
  }
  std::optional<ImportedLiteral> static_value;
  if (!is_null(fields->at("static_value"))) {
    const auto type_kind = type_kinds.find(*type);
    if (type_kind == type_kinds.end()) {
      issue(std::string{kRecord}, "static literal type is unknown");
      return std::nullopt;
    }
    auto literal =
        decode_literal(fields->at("static_value"), type_kind->second);
    if (!literal) return std::nullopt;
    static_value = std::move(*literal);
  }
  return ImportedMember{std::move(*id),
                        std::move(*owner),
                        *name,
                        *kind,
                        *visibility,
                        std::move(*type),
                        std::move(parameters),
                        std::move(*location),
                        *final,
                        *is_static,
                        *override,
                        *abstract,
                        slot,
                        std::move(*overridden),
                        std::move(*base),
                        std::move(static_value)};
}

bool MetadataDecoder::decode_declarations(
    const JsonValue& value, const PackageIdentity& package,
    const std::map<std::string, TypeKind, std::less<>>& type_kinds,
    std::vector<ImportedFile>& files) {
  const auto* values = array(value, "declarations");
  if (values == nullptr) return false;
  std::vector<ImportedMember> members;
  std::string previous_identity;
  bool first = true;
  for (const JsonValue& item : *values) {
    const auto* generic = std::get_if<JsonValue::Object>(&item.data);
    if (generic == nullptr || !generic->contains("record")) {
      issue("declarations", "declaration record is not an object");
      return false;
    }
    const std::string* record = text(generic->at("record"), "declarations");
    if (record == nullptr) return false;
    std::string identity_value;
    if (*record == "file") {
      auto file = decode_file_declaration(item, package);
      if (!file) return false;
      identity_value = file->identity;
      files.push_back(std::move(*file));
    } else if (*record == "member") {
      auto member = decode_member_declaration(item, type_kinds);
      if (!member) return false;
      identity_value = member->identity;
      members.push_back(std::move(*member));
    } else {
      issue("declarations", "unknown declaration record kind");
      return false;
    }
    if (!first && identity_value <= previous_identity) {
      issue("declarations", "declarations are not in canonical unique order");
      return false;
    }
    first = false;
    previous_identity = std::move(identity_value);
  }
  std::map<std::string, ImportedFile*, std::less<>> owners;
  for (ImportedFile& file : files) owners.emplace(file.identity, &file);
  for (ImportedMember& member : members) {
    const auto owner = owners.find(member.owner_identity);
    if (owner == owners.end()) {
      issue("declarations", "member owner is absent from the artifact");
      return false;
    }
    owner->second->members.push_back(std::move(member));
  }
  for (ImportedFile& file : files) {
    std::ranges::sort(file.members, {}, &ImportedMember::identity);
  }
  std::ranges::sort(files, {}, &ImportedFile::identity);
  return true;
}

std::optional<AbiLinkage> parse_linkage(std::string_view value) {
  if (value == "external") return AbiLinkage::kExternal;
  if (value == "internal") return AbiLinkage::kInternal;
  return std::nullopt;
}

std::optional<AbiParameterKind> parse_parameter_kind(std::string_view value) {
  if (value == "receiver") return AbiParameterKind::kReceiver;
  if (value == "result") return AbiParameterKind::kResult;
  if (value == "explicit") return AbiParameterKind::kExplicit;
  return std::nullopt;
}

std::optional<AbiCallableKind> parse_callable_kind(std::string_view value) {
  if (value == "function") return AbiCallableKind::kFunction;
  if (value == "constructor") return AbiCallableKind::kConstructor;
  return std::nullopt;
}

std::optional<AbiPassingMode> parse_passing_mode(std::string_view value) {
  if (value == "direct") return AbiPassingMode::kDirect;
  if (value == "value_pointer") return AbiPassingMode::kValuePointer;
  if (value == "result_pointer") return AbiPassingMode::kResultPointer;
  return std::nullopt;
}

std::optional<AbiReturnMode> parse_return_mode(std::string_view value) {
  if (value == "void") return AbiReturnMode::kVoid;
  if (value == "direct") return AbiReturnMode::kDirect;
  if (value == "indirect") return AbiReturnMode::kIndirect;
  return std::nullopt;
}

std::optional<AbiReceiverMode> parse_receiver_mode(std::string_view value) {
  if (value == "none") return AbiReceiverMode::kNone;
  if (value == "reference") return AbiReceiverMode::kReference;
  if (value == "readonly_value") return AbiReceiverMode::kReadOnlyValue;
  if (value == "construction") return AbiReceiverMode::kConstruction;
  return std::nullopt;
}

std::optional<ImportedAbiParameter> MetadataDecoder::decode_abi_parameter(
    const JsonValue& value) {
  const auto* fields =
      object(value, {"kind", "passing", "type"}, "layouts.parameter");
  if (fields == nullptr) return std::nullopt;
  const std::string* kind_text = text(fields->at("kind"), "layouts.parameter");
  auto type = identity(fields->at("type"), "layouts.parameter");
  const auto kind =
      kind_text == nullptr ? std::nullopt : parse_parameter_kind(*kind_text);
  const auto* passing_text = text(fields->at("passing"), "layouts.parameter");
  const auto passing =
      passing_text ? parse_passing_mode(*passing_text) : std::nullopt;
  if (!kind || !type || !passing) {
    issue("layouts.parameter", "ABI parameter is invalid");
    return std::nullopt;
  }
  return ImportedAbiParameter{*kind, std::move(*type), *passing};
}

std::optional<ImportedCallableAbi> MetadataDecoder::decode_callable(
    const JsonValue& value) {
  constexpr std::string_view kRecord = "layouts.callable";
  const auto* fields = object(
      value,
      {"calling_convention", "initializer", "kind", "linkage", "mangled_name",
       "member", "parameters", "receiver_mode", "return_mode", "return_type"},
      kRecord);
  if (fields == nullptr) return std::nullopt;
  const std::string* calling_convention =
      text(fields->at("calling_convention"), kRecord);
  const std::string* kind_text = text(fields->at("kind"), kRecord);
  const std::string* linkage_text = text(fields->at("linkage"), kRecord);
  const std::string* mangled = text(fields->at("mangled_name"), kRecord);
  auto member = identity(fields->at("member"), kRecord);
  auto return_type = identity(fields->at("return_type"), kRecord);
  const auto* return_text = text(fields->at("return_mode"), kRecord);
  const auto* receiver_text = text(fields->at("receiver_mode"), kRecord);
  const auto return_mode =
      return_text ? parse_return_mode(*return_text) : std::nullopt;
  const auto receiver_mode =
      receiver_text ? parse_receiver_mode(*receiver_text) : std::nullopt;
  const auto kind =
      kind_text == nullptr ? std::nullopt : parse_callable_kind(*kind_text);
  const auto linkage =
      linkage_text == nullptr ? std::nullopt : parse_linkage(*linkage_text);
  const JsonValue::Array* parameter_values =
      array(fields->at("parameters"), kRecord);
  if (calling_convention == nullptr || *calling_convention != "c" || !kind ||
      !linkage || mangled == nullptr || !member || !return_type ||
      !return_mode || !receiver_mode || parameter_values == nullptr) {
    issue(std::string{kRecord}, "callable ABI is invalid");
    return std::nullopt;
  }
  std::vector<ImportedAbiParameter> parameters;
  for (const JsonValue& item : *parameter_values) {
    auto parameter = decode_abi_parameter(item);
    if (!parameter) return std::nullopt;
    parameters.push_back(std::move(*parameter));
  }

  std::optional<std::string> initializer_identity;
  std::string initializer_mangled;
  AbiLinkage initializer_linkage = AbiLinkage::kInternal;
  std::optional<std::string> initializer_return;
  std::vector<ImportedAbiParameter> initializer_parameters;
  AbiReturnMode initializer_return_mode = AbiReturnMode::kVoid;
  AbiReceiverMode initializer_receiver_mode = AbiReceiverMode::kNone;
  if (!is_null(fields->at("initializer"))) {
    const auto* initializer =
        object(fields->at("initializer"),
               {"id", "linkage", "mangled_name", "parameters", "receiver_mode",
                "return_mode", "return_type"},
               "layouts.callable.initializer");
    if (initializer == nullptr) return std::nullopt;
    auto id = identity(initializer->at("id"), "layouts.callable.initializer");
    const std::string* initializer_linkage_text =
        text(initializer->at("linkage"), "layouts.callable.initializer");
    const std::string* initializer_name =
        text(initializer->at("mangled_name"), "layouts.callable.initializer");
    auto initializer_return_value = optional_identity(
        initializer->at("return_type"), "layouts.callable.initializer");
    const JsonValue::Array* initializer_parameter_values =
        array(initializer->at("parameters"), "layouts.callable.initializer");
    const auto parsed_initializer_linkage =
        initializer_linkage_text == nullptr
            ? std::nullopt
            : parse_linkage(*initializer_linkage_text);
    const auto* init_return_text =
        text(initializer->at("return_mode"), kRecord);
    const auto* init_receiver_text =
        text(initializer->at("receiver_mode"), kRecord);
    const auto init_return =
        init_return_text ? parse_return_mode(*init_return_text) : std::nullopt;
    const auto init_receiver = init_receiver_text
                                   ? parse_receiver_mode(*init_receiver_text)
                                   : std::nullopt;
    if (!init_return || !init_receiver || !id || !parsed_initializer_linkage ||
        initializer_name == nullptr || !initializer_return_value ||
        initializer_parameter_values == nullptr) {
      issue("layouts.callable.initializer",
            "constructor initializer ABI is invalid");
      return std::nullopt;
    }
    for (const JsonValue& item : *initializer_parameter_values) {
      auto parameter = decode_abi_parameter(item);
      if (!parameter) return std::nullopt;
      initializer_parameters.push_back(std::move(*parameter));
    }
    initializer_return_mode = *init_return;
    initializer_receiver_mode = *init_receiver;
    initializer_identity = std::move(*id);
    initializer_mangled = *initializer_name;
    initializer_linkage = *parsed_initializer_linkage;
    initializer_return = std::move(*initializer_return_value);
  }
  return ImportedCallableAbi{std::move(*member),
                             *kind,
                             *linkage,
                             AbiCallingConvention::kC,
                             *mangled,
                             std::move(initializer_identity),
                             std::move(initializer_mangled),
                             initializer_linkage,
                             std::move(initializer_return),
                             std::move(initializer_parameters),
                             std::move(*return_type),
                             std::move(parameters),
                             *return_mode,
                             *receiver_mode,
                             initializer_return_mode,
                             initializer_receiver_mode};
}

std::optional<ImportedTypeDescriptor> MetadataDecoder::decode_descriptor(
    const JsonValue& value) {
  constexpr std::string_view kRecord = "layouts.descriptor";
  const auto* fields = object(
      value,
      {"alignment", "display_name", "id", "interfaces", "kind", "mangled_name",
       "parent", "reference_offsets", "size", "virtual_functions"},
      kRecord);
  if (fields == nullptr) return std::nullopt;
  const auto alignment = integer(fields->at("alignment"), kRecord);
  const std::string* display = text(fields->at("display_name"), kRecord);
  auto id = identity(fields->at("id"), kRecord);
  const std::string* kind = text(fields->at("kind"), kRecord);
  const std::string* mangled = text(fields->at("mangled_name"), kRecord);
  auto parent = optional_identity(fields->at("parent"), kRecord);
  const auto size = integer(fields->at("size"), kRecord);
  auto virtuals = identity_array(fields->at("virtual_functions"), kRecord);
  const JsonValue::Array* reference_values =
      array(fields->at("reference_offsets"), kRecord);
  const JsonValue::Array* interface_values =
      array(fields->at("interfaces"), kRecord);
  if (!alignment || display == nullptr || !id || kind == nullptr ||
      *kind != "file_class" || mangled == nullptr || !parent || !size ||
      !virtuals || reference_values == nullptr || interface_values == nullptr) {
    issue(std::string{kRecord}, "descriptor ABI is invalid");
    return std::nullopt;
  }
  std::vector<std::uint64_t> references;
  for (const JsonValue& item : *reference_values) {
    const auto offset = integer(item, kRecord);
    if (!offset) return std::nullopt;
    references.push_back(*offset);
  }
  std::vector<ImportedInterfaceDispatch> interfaces;
  for (const JsonValue& item : *interface_values) {
    const auto* dispatch =
        object(item, {"functions", "id", "interface"}, kRecord);
    if (dispatch == nullptr) return std::nullopt;
    auto functions = identity_array(dispatch->at("functions"), kRecord);
    const auto interface_id = integer(dispatch->at("id"), kRecord);
    auto interface_identity = identity(dispatch->at("interface"), kRecord);
    if (!functions || !interface_id || !interface_identity) {
      return std::nullopt;
    }
    interfaces.push_back(ImportedInterfaceDispatch{
        std::move(*interface_identity), *interface_id, std::move(*functions)});
  }
  return ImportedTypeDescriptor{AbiHeapObjectKind::kFileClass,
                                std::move(*id),
                                std::move(*parent),
                                *display,
                                *size,
                                *alignment,
                                std::move(references),
                                std::move(*virtuals),
                                std::move(interfaces),
                                *mangled};
}

std::optional<ImportedClassAbi> MetadataDecoder::decode_layout(
    const JsonValue& value, std::string& owner) {
  constexpr std::string_view kRecord = "layouts";
  const auto* fields = object(value,
                              {"alignment", "callables", "descriptor", "fields",
                               "header_size", "owner", "size", "static_fields"},
                              kRecord);
  if (fields == nullptr) return std::nullopt;
  const auto alignment = integer(fields->at("alignment"), kRecord);
  std::optional<ImportedTypeDescriptor> descriptor;
  if (!is_null(fields->at("descriptor"))) {
    descriptor = decode_descriptor(fields->at("descriptor"));
    if (!descriptor) return std::nullopt;
  }
  const auto header = integer(fields->at("header_size"), kRecord);
  auto owner_identity = identity(fields->at("owner"), kRecord);
  const auto size = integer(fields->at("size"), kRecord);
  const JsonValue::Array* field_values = array(fields->at("fields"), kRecord);
  const JsonValue::Array* static_values =
      array(fields->at("static_fields"), kRecord);
  const JsonValue::Array* callable_values =
      array(fields->at("callables"), kRecord);
  if (!alignment || !header || !owner_identity || !size ||
      field_values == nullptr || static_values == nullptr ||
      callable_values == nullptr) {
    return std::nullopt;
  }
  std::vector<ImportedFieldLayout> imported_fields;
  for (const JsonValue& item : *field_values) {
    const auto* field = object(item, {"field", "offset", "type"}, kRecord);
    if (field == nullptr) return std::nullopt;
    auto field_identity = identity(field->at("field"), kRecord);
    const auto offset = integer(field->at("offset"), kRecord);
    auto type = identity(field->at("type"), kRecord);
    if (!field_identity || !offset || !type) return std::nullopt;
    imported_fields.push_back(ImportedFieldLayout{std::move(*field_identity),
                                                  std::move(*type), *offset});
  }
  std::vector<ImportedStaticFieldAbi> static_fields;
  for (const JsonValue& item : *static_values) {
    const auto* field =
        object(item, {"linkage", "mangled_name", "member", "type"}, kRecord);
    if (field == nullptr) return std::nullopt;
    const std::string* linkage_text = text(field->at("linkage"), kRecord);
    const std::string* mangled = text(field->at("mangled_name"), kRecord);
    auto member = identity(field->at("member"), kRecord);
    auto type = identity(field->at("type"), kRecord);
    const auto linkage =
        linkage_text == nullptr ? std::nullopt : parse_linkage(*linkage_text);
    if (!linkage || mangled == nullptr || !member || !type) {
      issue(std::string{kRecord}, "static field ABI is invalid");
      return std::nullopt;
    }
    static_fields.push_back(ImportedStaticFieldAbi{
        std::move(*member), std::move(*type), *linkage, *mangled});
  }
  std::vector<ImportedCallableAbi> callables;
  for (const JsonValue& item : *callable_values) {
    auto callable = decode_callable(item);
    if (!callable) return std::nullopt;
    callables.push_back(std::move(*callable));
  }
  owner = std::move(*owner_identity);
  return ImportedClassAbi{*header,
                          *size,
                          *alignment,
                          std::move(imported_fields),
                          std::move(descriptor),
                          std::move(static_fields),
                          std::move(callables)};
}

bool MetadataDecoder::decode_layouts(const JsonValue& value,
                                     std::vector<ImportedFile>& files) {
  const auto* values = array(value, "layouts");
  if (values == nullptr) return false;
  std::map<std::string, ImportedFile*, std::less<>> owners;
  for (ImportedFile& file : files) owners.emplace(file.identity, &file);
  std::set<std::string, std::less<>> assigned;
  std::string previous;
  bool first = true;
  for (const JsonValue& item : *values) {
    std::string owner;
    auto layout = decode_layout(item, owner);
    if (!layout) return false;
    if ((!first && owner <= previous) || !assigned.insert(owner).second) {
      issue("layouts", "layouts are not in canonical unique owner order");
      return false;
    }
    first = false;
    previous = owner;
    const auto file = owners.find(owner);
    if (file == owners.end()) {
      issue("layouts", "layout owner is absent from declarations");
      return false;
    }
    file->second->abi = std::move(*layout);
  }
  if (assigned.size() != files.size()) {
    issue("layouts", "every file declaration must have one layout");
    return false;
  }
  return true;
}

std::optional<ArtifactSymbolRole> parse_symbol_role(std::string_view value) {
  if (value == "definition") return ArtifactSymbolRole::kDefinition;
  if (value == "requirement") return ArtifactSymbolRole::kRequirement;
  return std::nullopt;
}

std::optional<ArtifactSymbolKind> parse_artifact_symbol_kind(
    std::string_view value) {
  if (value == "callable") return ArtifactSymbolKind::kCallable;
  if (value == "constructor_initializer") {
    return ArtifactSymbolKind::kConstructorInitializer;
  }
  if (value == "static_field") return ArtifactSymbolKind::kStaticField;
  if (value == "descriptor") return ArtifactSymbolKind::kDescriptor;
  if (value == "runtime") return ArtifactSymbolKind::kRuntime;
  return std::nullopt;
}

std::optional<std::vector<ArtifactSymbol>> MetadataDecoder::decode_symbols(
    const JsonValue& value) {
  const auto* values = array(value, "symbols");
  if (values == nullptr) return std::nullopt;
  std::vector<ArtifactSymbol> result;
  for (const JsonValue& item : *values) {
    const auto* fields =
        object(item, {"abi_signature", "identity", "kind", "link_name", "role"},
               "symbols");
    if (fields == nullptr) return std::nullopt;
    const std::string* signature = text(fields->at("abi_signature"), "symbols");
    auto symbol_identity = optional_identity(fields->at("identity"), "symbols");
    const std::string* kind_text = text(fields->at("kind"), "symbols");
    const std::string* link_name = text(fields->at("link_name"), "symbols");
    const std::string* role_text = text(fields->at("role"), "symbols");
    const auto kind = kind_text == nullptr
                          ? std::nullopt
                          : parse_artifact_symbol_kind(*kind_text);
    const auto role =
        role_text == nullptr ? std::nullopt : parse_symbol_role(*role_text);
    if (signature == nullptr || !symbol_identity || !kind ||
        link_name == nullptr || !role) {
      issue("symbols", "symbol inventory record is invalid");
      return std::nullopt;
    }
    result.push_back(ArtifactSymbol{*link_name, std::move(*symbol_identity),
                                    *role, *kind, *signature});
  }
  return result;
}

std::optional<PackageArtifact> MetadataDecoder::decode() {
  const auto* fields =
      object(root_,
             {"compatibility", "declarations", "dependencies", "kind",
              "layouts", "package", "sources", "symbols", "types"},
             "metadata");
  if (fields == nullptr) return std::nullopt;
  const std::string* kind_text = text(fields->at("kind"), "kind");
  PackageArtifactKind kind;
  if (kind_text != nullptr && *kind_text == "interface") {
    kind = PackageArtifactKind::kInterface;
  } else if (kind_text != nullptr && *kind_text == "object") {
    kind = PackageArtifactKind::kObject;
  } else {
    issue("kind", "unknown package artifact kind");
    return std::nullopt;
  }
  auto package = decode_package(fields->at("package"), "package");
  auto compatibility = decode_compatibility(fields->at("compatibility"));
  auto sources = decode_sources(fields->at("sources"));
  auto dependencies = decode_dependencies(fields->at("dependencies"));
  auto types = decode_types(fields->at("types"));
  auto symbols = decode_symbols(fields->at("symbols"));
  if (!package || !compatibility || !sources || !dependencies || !types ||
      !symbols) {
    return std::nullopt;
  }
  std::map<std::string, TypeKind, std::less<>> type_kinds;
  for (const ImportedType& type : *types) {
    type_kinds.emplace(type.identity, type.kind);
  }
  std::vector<ImportedFile> files;
  if (!decode_declarations(fields->at("declarations"), *package, type_kinds,
                           files) ||
      !decode_layouts(fields->at("layouts"), files)) {
    return std::nullopt;
  }
  TargetDataLayout target = compatibility->target;
  return PackageArtifact{
      kind,
      std::move(*compatibility),
      std::move(*sources),
      std::move(*dependencies),
      ImportedPackageView{std::move(*package), std::move(target),
                          std::move(*types), std::move(files)},
      std::move(*symbols),
      {}};
}

template <typename Value, typename Projection>
bool is_sorted_unique(const std::vector<Value>& values, Projection projection) {
  return std::ranges::is_sorted(values, {}, projection) &&
         std::adjacent_find(values.begin(), values.end(),
                            [&](const Value& left, const Value& right) {
                              return std::invoke(projection, left) ==
                                     std::invoke(projection, right);
                            }) == values.end();
}

bool valid_logical_source_path(std::string_view path) {
  if (path.empty() || !path.ends_with(".co") || path.starts_with('/') ||
      path.ends_with('/') || path.contains('\\') || path.contains("//") ||
      !valid_utf8(path)) {
    return false;
  }
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::string_view component =
        path.substr(begin, end == std::string_view::npos ? path.size() - begin
                                                         : end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

bool valid_nonempty_utf8(std::string_view value) {
  return !value.empty() && valid_utf8(value);
}

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset,
                       Endianness endianness) {
  if (endianness == Endianness::kLittle) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8U);
  }
  return static_cast<std::uint16_t>(bytes[offset] << 8U) |
         static_cast<std::uint16_t>(bytes[offset + 1]);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset,
                       Endianness endianness) {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    const std::size_t source =
        endianness == Endianness::kLittle ? offset + index : offset + 3 - index;
    result |= static_cast<std::uint32_t>(bytes[source])
              << static_cast<unsigned int>(index * 8);
  }
  return result;
}

bool valid_native_object(std::span<const std::uint8_t> payload,
                         const ArtifactNativeCompatibility& native,
                         const TargetDataLayout& target) {
  if (!target.target_name.starts_with("x86_64")) return false;
  if (native.object_format == "coff") {
    return payload.size() >= 20 &&
           read_u16(payload, 0, Endianness::kLittle) == 0x8664;
  }
  if (native.object_format == "elf") {
    if (payload.size() < 64 || payload[0] != 0x7f || payload[1] != 'E' ||
        payload[2] != 'L' || payload[3] != 'F' || payload[4] != 2 ||
        payload[6] != 1) {
      return false;
    }
    const Endianness endianness = payload[5] == 1   ? Endianness::kLittle
                                  : payload[5] == 2 ? Endianness::kBig
                                                    : target.endianness;
    return (payload[5] == 1 || payload[5] == 2) &&
           endianness == target.endianness &&
           read_u16(payload, 16, endianness) == 1 &&
           read_u16(payload, 18, endianness) == 62;
  }
  if (native.object_format == "mach_o") {
    if (payload.size() < 32) return false;
    const std::array<std::uint8_t, 4> little_magic{0xcf, 0xfa, 0xed, 0xfe};
    const std::array<std::uint8_t, 4> big_magic{0xfe, 0xed, 0xfa, 0xcf};
    const Endianness endianness =
        std::ranges::equal(little_magic, payload.first(4)) ? Endianness::kLittle
                                                           : Endianness::kBig;
    if (!std::ranges::equal(little_magic, payload.first(4)) &&
        !std::ranges::equal(big_magic, payload.first(4))) {
      return false;
    }
    return endianness == target.endianness &&
           read_u32(payload, 4, endianness) == 0x01000007U &&
           read_u32(payload, 12, endianness) == 1;
  }
  return false;
}

void append_issue(std::vector<ArtifactIssue>& issues, ArtifactIssueCode code,
                  std::string record, std::string message) {
  issues.push_back(ArtifactIssue{code, std::move(record), std::move(message)});
}

void write_little_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                      std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(
        value >> static_cast<unsigned int>(index * 8));
  }
}

void write_little_u64(std::vector<std::uint8_t>& bytes, std::size_t offset,
                      std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(
        value >> static_cast<unsigned int>(index * 8));
  }
}

std::uint32_t read_little_u32(std::span<const std::uint8_t> bytes,
                              std::size_t offset) {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    result |= static_cast<std::uint32_t>(bytes[offset + index])
              << static_cast<unsigned int>(index * 8);
  }
  return result;
}

std::uint64_t read_little_u64(std::span<const std::uint8_t> bytes,
                              std::size_t offset) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    result |= static_cast<std::uint64_t>(bytes[offset + index])
              << static_cast<unsigned int>(index * 8);
  }
  return result;
}

ArtifactDigest artifact_file_digest(std::span<const std::uint8_t> bytes) {
  artifact_internal::Sha256Hasher hasher;
  hasher.update(bytes.first(kDigestOffset));
  const std::array<std::uint8_t, kDigestSize> zeros{};
  hasher.update(zeros);
  if (bytes.size() > kDigestOffset + kDigestSize) {
    hasher.update(bytes.subspan(kDigestOffset + kDigestSize));
  }
  return hasher.finish();
}

ArtifactDigest stored_digest(std::span<const std::uint8_t> bytes) {
  ArtifactDigest result;
  std::ranges::copy(bytes.subspan(kDigestOffset, kDigestSize),
                    result.bytes.begin());
  return result;
}

}  // namespace

std::vector<ArtifactIssue> verify_package_artifact(
    const PackageArtifact& artifact) {
  std::vector<ArtifactIssue> issues;
  if (artifact.compatibility.compiler_abi != kCompilerAbiVersion) {
    append_issue(issues, ArtifactIssueCode::kIncompatible, "compatibility",
                 "compiler ABI revision is unsupported");
  }
  if (artifact.compatibility.runtime_abi != kRuntimeAbiVersion) {
    append_issue(issues, ArtifactIssueCode::kIncompatible, "compatibility",
                 "runtime ABI revision is unsupported");
  }
  if (artifact.compatibility.target != artifact.imported.target ||
      !is_valid_data_layout(artifact.compatibility.target)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "compatibility",
                 "target layout is invalid or disagrees with imported ABI");
  }
  if (artifact.kind == PackageArtifactKind::kInterface) {
    if (artifact.compatibility.native || !artifact.native_payload.empty()) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel, "kind",
                   "interface artifact has native configuration or payload");
    }
  } else if (!artifact.compatibility.native ||
             artifact.native_payload.empty()) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "kind",
                 "object artifact lacks native configuration or payload");
  } else if (!valid_native_object(artifact.native_payload,
                                  *artifact.compatibility.native,
                                  artifact.compatibility.target)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "payload",
                 "native payload format or machine is incompatible");
  }
  if (artifact.native_payload.size() > kMaximumArtifactPayloadSize) {
    append_issue(issues, ArtifactIssueCode::kLimitExceeded, "payload",
                 "native payload exceeds 1 GiB");
  }

  for (const ImportedPackageIssue& issue :
       verify_imported_package_view(artifact.imported)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, issue.record,
                 issue.message);
  }
  if (!is_sorted_unique(artifact.sources, &ArtifactSource::path)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "sources",
                 "sources are not in unique path order");
  }
  std::set<std::string, std::less<>> source_paths;
  for (const ArtifactSource& source : artifact.sources) {
    if (!valid_logical_source_path(source.path)) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel, "sources",
                   "source path is not a normalized package-relative .co path");
    }
    source_paths.insert(source.path);
  }
  std::set<std::string, std::less<>> declaration_paths;
  for (const ImportedFile& file : artifact.imported.files) {
    declaration_paths.insert(file.logical_path);
  }
  if (source_paths != declaration_paths) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "sources",
                 "source inventory does not match owned file declarations");
  }

  if (!is_sorted_unique(artifact.dependencies, &ArtifactDependency::alias)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "dependencies",
                 "dependencies are not in unique alias order");
  }
  std::set<std::string, std::less<>> dependency_packages;
  for (const ArtifactDependency& dependency : artifact.dependencies) {
    if (!is_valid_identifier(dependency.alias) ||
        !is_valid_package_name(dependency.package.name) ||
        !is_valid_package_version(dependency.package.version) ||
        dependency.package == artifact.imported.package ||
        !dependency_packages.insert(dependency.package.name).second) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel, "dependencies",
                   "dependency identity, alias, or ownership is invalid");
    }
  }

  const ArtifactNativeCompatibility* native =
      artifact.compatibility.native ? &*artifact.compatibility.native : nullptr;
  if (native != nullptr) {
    const bool format_matches_triple =
        (native->object_format == "coff" &&
         native->target_triple.contains("windows")) ||
        (native->object_format == "elf" &&
         !native->target_triple.contains("windows") &&
         !native->target_triple.contains("apple")) ||
        (native->object_format == "mach_o" &&
         native->target_triple.contains("apple"));
    if (!valid_nonempty_utf8(native->target_triple) ||
        !native->target_triple.starts_with("x86_64") ||
        !format_matches_triple || !valid_nonempty_utf8(native->object_format) ||
        !valid_nonempty_utf8(native->cpu) ||
        !valid_nonempty_utf8(native->relocation_model) ||
        !valid_nonempty_utf8(native->code_model) ||
        !is_sorted_unique(native->tools, &ArtifactToolIdentity::name) ||
        !std::ranges::is_sorted(native->features) ||
        std::adjacent_find(native->features.begin(), native->features.end()) !=
            native->features.end() ||
        std::ranges::any_of(native->features,
                            [](const std::string& feature) {
                              return !valid_nonempty_utf8(feature);
                            }) ||
        std::ranges::any_of(native->tools,
                            [](const ArtifactToolIdentity& tool) {
                              return !valid_nonempty_utf8(tool.name);
                            })) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel,
                   "compatibility.native",
                   "native configuration is empty, invalid, or noncanonical");
    }
  }

  if (!is_sorted_unique(artifact.symbols, &ArtifactSymbol::link_name)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "symbols",
                 "symbol inventory is not in unique link-name order");
  }
  std::map<std::string, ArtifactSymbolKind, std::less<>> expected_definitions;
  std::map<std::string, std::string, std::less<>> expected_signatures;
  for (const ImportedFile& file : artifact.imported.files) {
    if (file.abi.descriptor && !file.abi.descriptor->mangled_name.empty()) {
      expected_signatures.emplace(file.abi.descriptor->identity,
                                  "descriptor:file_class");
      expected_definitions.emplace(file.abi.descriptor->identity,
                                   ArtifactSymbolKind::kDescriptor);
    }
    for (const ImportedStaticFieldAbi& field : file.abi.static_fields) {
      if (field.linkage == AbiLinkage::kExternal) {
        expected_signatures.emplace(
            field.member_identity,
            "global:" + mangle_canonical_identity(field.type_identity));
        expected_definitions.emplace(field.member_identity,
                                     ArtifactSymbolKind::kStaticField);
      }
    }
    std::set<std::string, std::less<>> abstract_members;
    for (const ImportedMember& member : file.members) {
      if (member.is_abstract) abstract_members.insert(member.identity);
    }
    for (const ImportedCallableAbi& callable : file.abi.callables) {
      if (callable.linkage == AbiLinkage::kExternal &&
          !abstract_members.contains(callable.member_identity)) {
        expected_signatures.emplace(
            callable.member_identity,
            imported_callable_signature(
                callable.return_mode, callable.receiver_mode,
                callable.return_type_identity, callable.parameters));
        expected_definitions.emplace(callable.member_identity,
                                     ArtifactSymbolKind::kCallable);
      }
      if (callable.initializer_identity &&
          callable.initializer_linkage == AbiLinkage::kExternal) {
        if (callable.initializer_return_type_identity) {
          expected_signatures.emplace(
              *callable.initializer_identity,
              imported_callable_signature(
                  callable.initializer_return_mode,
                  callable.initializer_receiver_mode,
                  *callable.initializer_return_type_identity,
                  callable.initializer_parameters));
        }
        expected_definitions.emplace(
            *callable.initializer_identity,
            ArtifactSymbolKind::kConstructorInitializer);
      }
    }
  }
  std::set<std::string, std::less<>> actual_definitions;
  for (const ArtifactSymbol& symbol : artifact.symbols) {
    const bool runtime = symbol.kind == ArtifactSymbolKind::kRuntime;
    if (!valid_nonempty_utf8(symbol.link_name) ||
        !valid_nonempty_utf8(symbol.abi_signature) ||
        runtime == symbol.canonical_identity.has_value() ||
        (!runtime && symbol.link_name != mangle_canonical_identity(
                                             *symbol.canonical_identity))) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel, "symbols",
                   "symbol identity, name, kind, or signature is invalid");
    }
    if (runtime && symbol.role != ArtifactSymbolRole::kRequirement) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel, "symbols",
                   "runtime symbol is not a requirement");
    }
    if (symbol.role == ArtifactSymbolRole::kDefinition &&
        symbol.canonical_identity) {
      const auto expected =
          expected_definitions.find(*symbol.canonical_identity);
      if (expected == expected_definitions.end() ||
          expected->second != symbol.kind ||
          expected_signatures[*symbol.canonical_identity] !=
              symbol.abi_signature ||
          !actual_definitions.insert(*symbol.canonical_identity).second) {
        append_issue(issues, ArtifactIssueCode::kInvalidModel, "symbols",
                     "defined symbol is not owned by the package ABI");
      }
    }
    if (symbol.role == ArtifactSymbolRole::kRequirement &&
        symbol.canonical_identity &&
        expected_definitions.contains(*symbol.canonical_identity)) {
      append_issue(issues, ArtifactIssueCode::kInvalidModel, "symbols",
                   "owned symbol is listed as an external requirement");
    }
  }
  if (actual_definitions.size() != expected_definitions.size()) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "symbols",
                 "defined symbol inventory is incomplete");
  }
  if (!encode_metadata(artifact)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "declarations",
                 "typed static literal cannot be canonically encoded");
  }
  return issues;
}

PackageArtifactWriteResult write_package_artifact(
    const PackageArtifact& artifact) {
  std::vector<ArtifactIssue> issues = verify_package_artifact(artifact);
  if (!issues.empty()) {
    return PackageArtifactWriteResult{std::nullopt, std::move(issues)};
  }
  const auto metadata_value = encode_metadata(artifact);
  if (!metadata_value) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "metadata",
                 "metadata cannot be encoded");
    return PackageArtifactWriteResult{std::nullopt, std::move(issues)};
  }
  const std::string metadata = canonical_json(*metadata_value);
  if (!valid_utf8(metadata)) {
    append_issue(issues, ArtifactIssueCode::kInvalidModel, "metadata",
                 "metadata contains a string that is not valid UTF-8");
    return PackageArtifactWriteResult{std::nullopt, std::move(issues)};
  }
  if (metadata.size() > kMaximumArtifactMetadataSize) {
    append_issue(issues, ArtifactIssueCode::kLimitExceeded, "metadata",
                 "metadata exceeds 64 MiB");
    return PackageArtifactWriteResult{std::nullopt, std::move(issues)};
  }
  const std::uint64_t total_size =
      kArtifactHeaderSize + static_cast<std::uint64_t>(metadata.size()) +
      static_cast<std::uint64_t>(artifact.native_payload.size());
  if (total_size > std::numeric_limits<std::size_t>::max()) {
    append_issue(issues, ArtifactIssueCode::kLimitExceeded, "artifact",
                 "artifact cannot be represented on this host");
    return PackageArtifactWriteResult{std::nullopt, std::move(issues)};
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total_size), 0);
  std::ranges::copy(kArtifactMagic, bytes.begin());
  write_little_u32(bytes, 8, kPackageArtifactFormatVersion);
  write_little_u32(bytes, 12, 0);
  write_little_u64(bytes, 16, metadata.size());
  write_little_u64(bytes, 24, artifact.native_payload.size());
  std::transform(
      metadata.begin(), metadata.end(), bytes.begin() + kArtifactHeaderSize,
      [](char character) { return static_cast<std::uint8_t>(character); });
  const std::size_t payload_offset = kArtifactHeaderSize + metadata.size();
  std::ranges::copy(
      artifact.native_payload,
      bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset));
  const ArtifactDigest digest = artifact_file_digest(bytes);
  std::ranges::copy(digest.bytes, bytes.begin() + kDigestOffset);
  return PackageArtifactWriteResult{
      EncodedPackageArtifact{std::move(bytes), digest}, {}};
}

PackageArtifactReadResult read_package_artifact(
    std::span<const std::uint8_t> bytes,
    const std::optional<ArtifactCompatibility>& expected_compatibility) {
  std::vector<ArtifactIssue> issues;
  const auto fail = [&](ArtifactIssueCode code, std::string record,
                        std::string message) {
    append_issue(issues, code, std::move(record), std::move(message));
    return PackageArtifactReadResult{std::nullopt, std::nullopt,
                                     std::move(issues)};
  };
  if (bytes.size() < kArtifactHeaderSize) {
    return fail(ArtifactIssueCode::kMalformedEnvelope, "header",
                "artifact is shorter than the 64-byte header");
  }
  if (!std::ranges::equal(kArtifactMagic, bytes.first(kArtifactMagic.size()))) {
    return fail(ArtifactIssueCode::kMalformedEnvelope, "header",
                "artifact magic is invalid");
  }
  if (read_little_u32(bytes, 8) != kPackageArtifactFormatVersion) {
    return fail(ArtifactIssueCode::kIncompatible, "header",
                "artifact format version is unsupported");
  }
  if (read_little_u32(bytes, 12) != 0) {
    return fail(ArtifactIssueCode::kMalformedEnvelope, "header",
                "artifact flags are nonzero");
  }
  const std::uint64_t metadata_size = read_little_u64(bytes, 16);
  const std::uint64_t payload_size = read_little_u64(bytes, 24);
  if (metadata_size > kMaximumArtifactMetadataSize ||
      payload_size > kMaximumArtifactPayloadSize) {
    return fail(ArtifactIssueCode::kLimitExceeded, "header",
                "artifact section exceeds its version-1 limit");
  }
  if (metadata_size >
          std::numeric_limits<std::uint64_t>::max() - kArtifactHeaderSize ||
      payload_size > std::numeric_limits<std::uint64_t>::max() -
                         kArtifactHeaderSize - metadata_size) {
    return fail(ArtifactIssueCode::kMalformedEnvelope, "header",
                "artifact section sizes overflow");
  }
  const std::uint64_t expected_size =
      kArtifactHeaderSize + metadata_size + payload_size;
  if (expected_size != bytes.size()) {
    return fail(ArtifactIssueCode::kMalformedEnvelope, "header",
                "artifact is truncated or has trailing bytes");
  }
  const ArtifactDigest digest = stored_digest(bytes);
  if (artifact_file_digest(bytes) != digest) {
    return fail(ArtifactIssueCode::kIntegrityMismatch, "digest",
                "artifact SHA-256 digest does not match its bytes");
  }
  const auto metadata_bytes = bytes.subspan(
      kArtifactHeaderSize, static_cast<std::size_t>(metadata_size));
  const std::string_view metadata{
      reinterpret_cast<const char*>(metadata_bytes.data()),
      metadata_bytes.size()};
  JsonParser parser{metadata};
  auto root = parser.parse();
  if (!root) {
    return fail(ArtifactIssueCode::kMalformedMetadata, "metadata",
                parser.error());
  }
  MetadataDecoder decoder{*root};
  auto artifact = decoder.decode();
  std::vector<ArtifactIssue> decode_issues = decoder.take_issues();
  if (!artifact || !decode_issues.empty()) {
    if (decode_issues.empty()) {
      append_issue(decode_issues, ArtifactIssueCode::kMalformedMetadata,
                   "metadata", "metadata record could not be reconstructed");
    }
    return PackageArtifactReadResult{std::nullopt, digest,
                                     std::move(decode_issues)};
  }
  const std::size_t payload_offset =
      kArtifactHeaderSize + static_cast<std::size_t>(metadata_size);
  artifact->native_payload.assign(
      bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset), bytes.end());
  std::vector<ArtifactIssue> model_issues = verify_package_artifact(*artifact);
  if (!model_issues.empty()) {
    return PackageArtifactReadResult{std::nullopt, digest,
                                     std::move(model_issues)};
  }
  const auto canonical = encode_metadata(*artifact);
  if (!canonical || canonical_json(*canonical) != metadata) {
    append_issue(issues, ArtifactIssueCode::kNoncanonicalMetadata, "metadata",
                 "metadata does not use the canonical version-1 encoding");
    return PackageArtifactReadResult{std::nullopt, digest, std::move(issues)};
  }
  if (expected_compatibility &&
      artifact->compatibility != *expected_compatibility) {
    append_issue(issues, ArtifactIssueCode::kIncompatible, "compatibility",
                 "artifact compatibility does not exactly match the build");
    return PackageArtifactReadResult{std::nullopt, digest, std::move(issues)};
  }
  return PackageArtifactReadResult{std::move(artifact), digest, {}};
}

}  // namespace cloth
