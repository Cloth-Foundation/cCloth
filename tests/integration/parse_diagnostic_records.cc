// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "parse_diagnostic_records.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ostream>
#include <string_view>
#include <vector>

namespace cloth::test {
namespace {

struct CanonicalDiagnostic {
  int kind;
  SourceRange range;
  std::optional<SourceRange> related_range;
};

void write_range(std::ostream& output, const SourceRange& range) {
  output << range.begin.byte_offset << ',' << range.begin.line << ','
         << range.begin.column << ',' << range.end.byte_offset << ','
         << range.end.line << ',' << range.end.column;
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
  if (message == "expected a file type after declaration modifiers") return 12;
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
  if (message == "expected a type" ||
      message == "expected an element type before '['") {
    return 25;
  }
  if (message == "nullable qualification cannot be repeated") return 26;
  if (message == "multidimensional array types are not supported") return 27;
  if (message == "expected ']' in array type") return 28;
  if (starts_with(message, "expected '(' after '") &&
      message != "expected '(' after 'if'" &&
      message != "expected '(' after 'while'" &&
      message != "expected '(' after 'for'" &&
      message != "expected '(' after 'switch'") {
    return 29;
  }
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
  if (message == "expected ';' after abstract function declaration") return 42;
  if (starts_with(message, "expected '{' to begin body of '")) return 43;
  if (starts_with(message, "unterminated body for '")) return 44;
  if (message == "expected an error type after 'throws'") return 45;
  if (message == "expected an error type after ',' in throws clause") return 46;
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
  if (starts_with(message, "nested type declarations are reserved")) return 53;
  if (message == "expected expression" ||
      message == "expected expression after 'throw'") {
    return 55;
  }
  if (message == "unexpected token in field initializer" ||
      message == "expected constructor body after base initializer") {
    return 56;
  }
  if (message == "expected a type after checked type operator") return 57;
  if (message == "expected ')' after parenthesized expression") return 58;
  if (message == "expected argument after ','") return 59;
  if (message == "expected ',' or ')' after argument") return 60;
  if (message == "expected ')' after arguments") return 61;
  if (message == "expected index expression after '['") return 62;
  if (message == "expected ']' after index expression") return 63;
  if (message == "expected member name after '.'") return 64;
  if (message == "expected meta query name after '::'") return 65;
  if (message == "expected member name after '?.'") return 66;
  if (message == "expected meta query name after '?::'") return 67;
  if (message == "expected array element after ','") return 68;
  if (message == "expected ',' or ']' after array element") return 69;
  if (message == "expected ']' after array literal") return 70;
  if (message == "expected '[' in array construction") return 71;
  if (message == "expected ':' after '[' in array construction") return 72;
  if (message == "expected length expression after ':'") return 73;
  if (message == "expected ']' after array length") return 74;
  if (message == "expected '(' after numeric conversion type") return 75;
  if (message == "expected a value in numeric conversion") return 76;
  if (message == "numeric conversion requires exactly one value") return 77;
  if (message == "expected ')' after numeric conversion") return 78;
  if (message == "expected integer conversion mode after '::'") return 79;
  if (message == "expected '(' after integer conversion mode") return 80;
  if (message == "expected a value in integer conversion") return 81;
  if (message == "integer conversion requires exactly one value") return 82;
  if (message == "expected ')' after integer conversion") return 83;
  if (message == "expected base class name in constructor initializer")
    return 84;
  if (message ==
      "expected '(' after base class name in constructor initializer") {
    return 85;
  }
  if (message == "expected ')' after base constructor arguments") return 88;
  if (message == "package exceeds 65536 static constants") return 89;
  if (message == "constant initializer exceeds 65536 expression nodes") {
    return 90;
  }
  if (message == "package exceeds 1048576 constant expression nodes") {
    return 91;
  }
  if (message == "constant expression exceeds nesting depth 256") return 92;
  if (message == "constant numeric literal exceeds 4096 bytes") return 93;
  if (message == "expected '{' to begin block") return 94;
  if (message == "switch labels are only valid directly inside a switch") {
    return 95;
  }
  if (message == "expected a local variable name") return 96;
  if (message == "expected ';' after local variable declaration") return 97;
  if (message == "expected ';' after return statement") return 98;
  if (message == "expected '(' after 'if'") return 99;
  if (message == "expected ')' after if condition") return 100;
  if (message == "expected '{' to begin if body") return 101;
  if (message == "expected '{' to begin else body") return 102;
  if (message == "expected '(' after 'while'") return 103;
  if (message == "expected ')' after while condition") return 104;
  if (message == "expected '{' to begin while body") return 105;
  if (message == "expected '(' after 'for'") return 106;
  if (message == "expected ';' after for initializer") return 107;
  if (message == "expected ';' after for condition") return 108;
  if (message == "expected ')' after for clauses" ||
      message == "expected ')' after for iterable") {
    return 109;
  }
  if (message == "expected '{' to begin for body") return 110;
  if (message == "expected '(' after 'switch'") return 111;
  if (message == "expected ')' after switch selector" ||
      message == "expected '}' after switch arms") {
    return 112;
  }
  if (message == "expected '{' before switch arms") return 113;
  if (message == "expected 'case' or 'default' in switch") return 114;
  if (message == "switch may have only one default arm") return 115;
  if (message == "default must be the last switch arm") return 116;
  if (message == "expected case label (no trailing comma)") return 117;
  if (message == "switch exceeds 65536 value labels") return 118;
  if (message == "expected ':' after switch label") return 119;
  if (message == "expected '{' before switch arm body") return 120;
  if (message == "switch exceeds 65537 arms") return 121;
  if (message == "switch requires at least one arm") return 122;
  if (starts_with(message, "expected ';' after '")) return 123;
  if (message == "expected ';' after expression statement") return 124;
  return std::nullopt;
}

}  // namespace

bool write_parse_diagnostic_records(std::span<const Diagnostic> diagnostics,
                                    std::string_view tag, std::ostream& output,
                                    std::ostream& errors,
                                    std::string_view context) {
  std::vector<CanonicalDiagnostic> canonical;
  for (std::size_t index = 0; index < diagnostics.size(); ++index) {
    const Diagnostic& value = diagnostics[index];
    if (value.severity == DiagnosticSeverity::kNote) continue;
    const std::optional<int> kind = diagnostic_kind(value.message);
    if (!kind) {
      errors << "unclassified " << context << " diagnostic: " << value.message
             << '\n';
      return false;
    }
    std::optional<SourceRange> related_range;
    if (index + 1 < diagnostics.size() &&
        diagnostics[index + 1].severity == DiagnosticSeverity::kNote) {
      related_range = diagnostics[index + 1].range;
    }
    canonical.push_back(CanonicalDiagnostic{*kind, value.range, related_range});
  }
  std::ranges::stable_sort(canonical, {}, [](const CanonicalDiagnostic& value) {
    return value.range.begin.byte_offset;
  });
  for (std::size_t index = 0; index < canonical.size(); ++index) {
    const CanonicalDiagnostic& value = canonical[index];
    output << tag << '|' << index << '|' << value.kind << '|';
    write_range(output, value.range);
    output << '|';
    if (value.related_range) {
      write_range(output, *value.related_range);
    } else {
      output << '-';
    }
    output << '\n';
  }
  return true;
}

}  // namespace cloth::test
