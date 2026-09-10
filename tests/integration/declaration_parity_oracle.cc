// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/lexer.h"
#include "cloth/parser/declaration_pass.h"
#include "cloth/source/source_file.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct CanonicalEnumCase {
  std::string_view name;
  cloth::SourceRange range;
  bool is_valid;
};

struct CanonicalDiagnostic {
  int kind;
  cloth::SourceRange range;
  std::optional<cloth::SourceRange> related_range;
};

void write_flag(bool value) { std::cout << (value ? 1 : 0); }

void write_range(const cloth::SourceRange& range) {
  std::cout << range.begin.byte_offset << ',' << range.begin.line << ','
            << range.begin.column << ',' << range.end.byte_offset << ','
            << range.end.line << ',' << range.end.column;
}

void write_bytes(std::string_view value) {
  if (value.empty()) {
    std::cout << '-';
    return;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << static_cast<unsigned int>(
        static_cast<unsigned char>(value[index]));
  }
}

void write_interval(const std::optional<cloth::TokenIndexRange>& value) {
  if (!value) {
    std::cout << '-';
    return;
  }
  std::cout << value->begin << ',' << value->end;
}

[[nodiscard]] int visibility_code(cloth::Visibility value) {
  return value == cloth::Visibility::kPrivate ? 0 : 1;
}

void write_type_value(const cloth::TypeSyntax& value) {
  write_flag(value.is_primitive);
  std::cout << '|';
  write_flag(value.is_array);
  std::cout << '|';
  write_flag(value.is_nullable);
  std::cout << '|';
  write_flag(value.is_element_nullable);
  std::cout << '|';
  write_range(value.range);
  std::cout << '|';
  write_bytes(value.name);
}

void write_type(char category, std::size_t index,
                const cloth::TypeSyntax& value) {
  std::cout << category << '|' << index << '|';
  write_type_value(value);
  std::cout << '\n';
}

void write_member_type(char category, std::size_t member_index,
                       std::size_t index, const cloth::TypeSyntax& value) {
  std::cout << category << '|' << member_index << '|' << index << '|';
  write_type_value(value);
  std::cout << '\n';
}

[[nodiscard]] bool starts_with(std::string_view value,
                               std::string_view prefix) {
  return value.starts_with(prefix);
}

[[nodiscard]] std::optional<int> diagnostic_kind(std::string_view message) {
  if (starts_with(message, "source file stem '")) return 0;
  if (starts_with(message, "expected a field, function, or constructor")) {
    return 1;
  }
  if (starts_with(message, "imports must appear before")) return 2;
  if (starts_with(message, "expected a package or type name")) return 3;
  if (message == "expected a type name after '::'") return 4;
  if (starts_with(message, "expected a package name or '*' after") ||
      starts_with(message, "expected '::' for a type")) {
    return 5;
  }
  if (message == "standard library root must be spelled 'cloth'") return 6;
  if (starts_with(message, "import alias '") &&
      message.ends_with("standard library root 'cloth'")) {
    return 7;
  }
  if (message == "expected an alias name after 'as'") return 8;
  if (message == "wildcard imports cannot have an alias") return 9;
  if (message == "expected ';' after import") return 10;
  if (starts_with(message, "duplicate '") &&
      message.ends_with("class modifier")) {
    return 11;
  }
  if (message == "expected a file type after declaration modifiers") {
    return 12;
  }
  if (message ==
      "interfaces, enums, and structs cannot be declared abstract or sealed") {
    return 13;
  }
  if (starts_with(message, "the source file already defines implicit type")) {
    return 14;
  }
  if (message == "expected a file class name after ':'") return 15;
  if (starts_with(message, "expected an interface name")) return 16;
  if (message == "expected '{' after file type declaration") return 17;
  if (message == "expected '}' to close file type declaration") return 18;
  if (message == "expected end of file after file type declaration") return 19;
  if (message == "expected an enum case name") return 20;
  if (starts_with(message, "duplicate enum case '")) return 21;
  if (message == "enum exceeds the 65536-case limit") return 22;
  if (message == "enum must declare at least one case") return 23;
  if (message == "expected ',' or '}' after enum case") return 24;
  if (message == "expected a type") return 25;
  if (message == "nullable qualification cannot be repeated") return 26;
  if (message == "multidimensional array types are not supported") return 27;
  if (message == "expected ']' in array type") return 28;
  if (starts_with(message, "expected '(' after '")) return 29;
  if (starts_with(message, "expected parameter name after type")) return 30;
  if (message == "expected ',' or ')' after parameter") return 31;
  if (message == "expected parameter after ','") return 32;
  if (message == "expected ')' after parameter list") return 33;
  if (starts_with(message, "expected field name after type")) return 34;
  if (message == "expected expression after '=' in field declaration") {
    return 35;
  }
  if (message == "expected ';' after field declaration") return 36;
  if (starts_with(message, "duplicate '") &&
      message.ends_with("function modifier")) {
    return 37;
  }
  if (message ==
      "interface function contracts do not accept function modifiers") {
    return 38;
  }
  if (message == "expected 'func' after function modifiers") return 39;
  if (message == "expected function name after 'func'") return 40;
  if (starts_with(message, "abstract function '") &&
      message.ends_with("cannot have a body")) {
    return 41;
  }
  if (message == "expected ';' after abstract function declaration") {
    return 42;
  }
  if (starts_with(message, "expected '{' to begin body of '")) return 43;
  if (starts_with(message, "unterminated body for '")) return 44;
  if (message == "expected an error type after 'throws'") return 45;
  if (message == "expected an error type after ',' in throws clause") {
    return 46;
  }
  if (starts_with(message, "constructor '") &&
      message.find("must use a class-derived name") != std::string_view::npos) {
    return 47;
  }
  if (message == "interfaces cannot declare constructors") return 48;
  if (message == "interfaces cannot declare fields") return 49;
  if (message == "expected base constructor after ':'") return 50;
  if (starts_with(message, "member '") &&
      message.find("conflicts with previous") != std::string_view::npos) {
    return 51;
  }
  if (starts_with(message, "duplicate function signature") ||
      starts_with(message, "duplicate constructor signature")) {
    return 52;
  }
  if (starts_with(message, "nested type declarations are reserved")) {
    return 53;
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<CanonicalEnumCase> canonical_enum_cases(
    const cloth::DeclarationPassResult& result,
    std::span<const cloth::Token> tokens) {
  std::vector<CanonicalEnumCase> cases;
  if (result.file_type_kind != cloth::FileTypeKind::kEnum) return cases;

  std::size_t index = 0;
  while (index < tokens.size() &&
         tokens[index].kind != cloth::TokenKind::kKwEnum) {
    ++index;
  }
  while (index < tokens.size() &&
         tokens[index].kind != cloth::TokenKind::kLeftBrace) {
    ++index;
  }
  if (index < tokens.size()) ++index;

  std::map<std::string_view, cloth::SourceRange, std::less<>> names;
  std::size_t unique_count = 0;
  while (index < tokens.size() &&
         tokens[index].kind != cloth::TokenKind::kRightBrace &&
         tokens[index].kind != cloth::TokenKind::kEof) {
    const cloth::Token& token = tokens[index];
    if (token.kind == cloth::TokenKind::kIdentifier) {
      const bool inserted = names.emplace(token.lexeme, token.range).second;
      if (!inserted) {
        cases.push_back(CanonicalEnumCase{token.lexeme, token.range, false});
      } else if (unique_count < cloth::kMaxEnumCases) {
        cases.push_back(CanonicalEnumCase{token.lexeme, token.range, true});
        ++unique_count;
      }
      ++index;
      if (index < tokens.size() &&
          tokens[index].kind == cloth::TokenKind::kRightBrace) {
        continue;
      }
      if (index < tokens.size() &&
          tokens[index].kind == cloth::TokenKind::kComma) {
        ++index;
        continue;
      }
    }

    std::size_t brace_depth = 0;
    while (index < tokens.size() &&
           tokens[index].kind != cloth::TokenKind::kEof) {
      const cloth::TokenKind kind = tokens[index].kind;
      if (brace_depth == 0 && (kind == cloth::TokenKind::kComma ||
                               kind == cloth::TokenKind::kRightBrace)) {
        break;
      }
      if (kind == cloth::TokenKind::kLeftBrace) ++brace_depth;
      if (kind == cloth::TokenKind::kRightBrace && brace_depth != 0) {
        --brace_depth;
      }
      ++index;
    }
    if (index < tokens.size() &&
        tokens[index].kind == cloth::TokenKind::kComma) {
      ++index;
    }
  }
  return cases;
}

int write_records(const std::filesystem::path& path) {
  auto loaded = cloth::SourceFile::load(path);
  if (!loaded) {
    std::cerr << path.string() << ": " << loaded.error().message << '\n';
    return 1;
  }
  cloth::SourceFile source = std::move(*loaded);
  cloth::DiagnosticEngine diagnostics;
  const std::vector<cloth::Token> tokens =
      cloth::Lexer{source, diagnostics}.lex();
  if (diagnostics.has_errors()) {
    std::cerr << "declaration record input has lexical diagnostics\n";
    return 1;
  }
  cloth::DeclarationPassResult result =
      cloth::DeclarationPass{source, tokens, diagnostics}.run();

  std::cout << "F|";
  write_flag(result.is_valid);
  std::cout << '|';
  write_flag(result.has_explicit_class_declaration);
  std::cout << '|' << static_cast<int>(result.file_type_kind) << '|';
  write_flag(result.is_abstract);
  std::cout << '|';
  write_flag(result.is_sealed);
  std::cout << '|' << visibility_code(result.symbols.visibility()) << '|';
  write_range(result.symbols.range());
  std::cout << '\n';

  for (std::size_t index = 0; index < result.imports.size(); ++index) {
    const cloth::ImportDecl& value = result.imports[index];
    std::cout << "I|" << index << '|' << static_cast<int>(value.kind) << '|';
    write_flag(value.is_valid);
    std::cout << '|';
    write_range(value.range);
    std::cout << '|';
    write_bytes(value.package_name);
    std::cout << '|';
    write_bytes(value.type_name);
    std::cout << '|';
    write_bytes(value.local_name);
    std::cout << '\n';
  }
  if (result.base_class) write_type('B', 0, *result.base_class);
  for (std::size_t index = 0; index < result.interfaces.size(); ++index) {
    write_type('N', index, result.interfaces[index]);
  }
  const std::vector<CanonicalEnumCase> enum_cases =
      canonical_enum_cases(result, tokens);
  for (std::size_t index = 0; index < enum_cases.size(); ++index) {
    const CanonicalEnumCase& value = enum_cases[index];
    std::cout << "E|" << index << '|';
    write_flag(value.is_valid);
    std::cout << '|';
    write_range(value.range);
    std::cout << '|';
    write_bytes(value.name);
    std::cout << '\n';
  }
  for (std::size_t index = 0; index < result.outlines.size(); ++index) {
    const cloth::MemberOutline& outline = result.outlines[index];
    const cloth::MemberSymbol& value =
        result.symbols.member(outline.symbol_index);
    std::cout << "M|" << index << '|' << static_cast<int>(value.kind) << '|';
    write_flag(value.is_valid);
    std::cout << '|' << visibility_code(value.visibility) << '|';
    write_flag(value.is_static);
    std::cout << '|';
    write_flag(value.is_final);
    std::cout << '|';
    write_flag(value.is_override);
    std::cout << '|';
    write_flag(value.is_abstract);
    std::cout << '|';
    write_flag(value.has_explicit_throws);
    std::cout << '|' << outline.begin_token << '|';
    write_range(outline.range);
    std::cout << '|';
    write_bytes(value.name);
    std::cout << '|';
    write_interval(outline.initializer_tokens);
    std::cout << '|';
    write_interval(outline.constructor_initializer_tokens);
    std::cout << '|';
    write_interval(outline.body_tokens);
    std::cout << '\n';
    if (value.declared_type) write_type('R', index, *value.declared_type);
    for (std::size_t parameter_index = 0;
         parameter_index < value.parameters.size(); ++parameter_index) {
      const cloth::ParameterSymbol& parameter =
          value.parameters[parameter_index];
      std::cout << "P|" << index << '|' << parameter_index << '|';
      write_flag(parameter.is_final);
      std::cout << '|';
      write_range(parameter.range);
      std::cout << '|';
      write_bytes(parameter.name);
      std::cout << '|';
      write_type_value(parameter.type);
      std::cout << '\n';
    }
    for (std::size_t throw_index = 0; throw_index < value.throws_types.size();
         ++throw_index) {
      write_member_type('H', index, throw_index,
                        value.throws_types[throw_index]);
    }
  }

  const std::span<const cloth::Diagnostic> diagnostic_values =
      diagnostics.diagnostics();
  std::vector<CanonicalDiagnostic> canonical_diagnostics;
  for (std::size_t index = 0; index < diagnostic_values.size(); ++index) {
    const cloth::Diagnostic& value = diagnostic_values[index];
    if (value.severity == cloth::DiagnosticSeverity::kNote) continue;
    const std::optional<int> kind = diagnostic_kind(value.message);
    if (!kind) {
      std::cerr << "unclassified declaration diagnostic: " << value.message
                << '\n';
      return 1;
    }
    std::optional<cloth::SourceRange> related_range;
    if (index + 1 < diagnostic_values.size() &&
        diagnostic_values[index + 1].severity ==
            cloth::DiagnosticSeverity::kNote) {
      related_range = diagnostic_values[index + 1].range;
    }
    canonical_diagnostics.push_back(
        CanonicalDiagnostic{*kind, value.range, related_range});
  }
  std::ranges::stable_sort(canonical_diagnostics, {},
                           [](const CanonicalDiagnostic& value) {
                             return value.range.begin.byte_offset;
                           });
  for (std::size_t index = 0; index < canonical_diagnostics.size(); ++index) {
    const CanonicalDiagnostic& value = canonical_diagnostics[index];
    std::cout << "D|" << index << '|' << value.kind << '|';
    write_range(value.range);
    std::cout << '|';
    if (value.related_range) {
      write_range(*value.related_range);
    } else {
      std::cout << '-';
    }
    std::cout << '\n';
  }
  return 0;
}

[[nodiscard]] bool generate_enum(const std::filesystem::path& path,
                                 std::size_t count) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << "enum {\n";
  for (std::size_t index = 0; index < count; ++index) {
    output << "  C" << index;
    if (index + 1 != count) output << ',';
    output << '\n';
  }
  output << "}\n";
  return output.good();
}

[[nodiscard]] bool generate_many_members(const std::filesystem::path& path) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  for (std::size_t index = 0; index < 4096; ++index) {
    output << "int32 field" << index << ";\n";
  }
  return output.good();
}

[[nodiscard]] bool generate_deep_regions(const std::filesystem::path& path) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << "int32 Value = ";
  for (std::size_t index = 0; index < 8192; ++index) output << '(';
  output << '1';
  for (std::size_t index = 0; index < 8192; ++index) output << ')';
  output << ";\nfunc Run(): void {\n";
  for (std::size_t index = 0; index < 8192; ++index) output << '{';
  for (std::size_t index = 0; index < 8192; ++index) output << '}';
  output << "\n}\n";
  return output.good();
}

int generate(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::cerr << directory.string() << ": " << error.message() << '\n';
    return 1;
  }
  if (!generate_enum(directory / "EnumLimit.co", cloth::kMaxEnumCases) ||
      !generate_enum(directory / "EnumOverLimit.co",
                     cloth::kMaxEnumCases + 1) ||
      !generate_many_members(directory / "ManyMembers.co") ||
      !generate_deep_regions(directory / "DeepRegions.co")) {
    std::cerr << "could not write generated declaration corpus\n";
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) return write_records(argv[1]);
  if (argc == 3 && std::string_view{argv[1]} == "generate") {
    return generate(argv[2]);
  }
  std::cerr << "usage: cloth_declaration_parity_oracle [generate] <path>\n";
  return 2;
}
