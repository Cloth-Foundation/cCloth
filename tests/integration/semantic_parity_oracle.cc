// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void write_flag(bool value) { std::cout << (value ? 1 : 0); }

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

int visibility_code(cloth::Visibility value) {
  return value == cloth::Visibility::kPrivate ? 0 : 1;
}

int nominal_kind_code(cloth::NominalKind value) {
  switch (value) {
    case cloth::NominalKind::kClass:
      return 0;
    case cloth::NominalKind::kInterface:
      return 1;
    case cloth::NominalKind::kEnum:
      return 2;
    case cloth::NominalKind::kStruct:
      return 3;
  }
  return -1;
}

int file_kind_code(cloth::FileTypeKind value) {
  switch (value) {
    case cloth::FileTypeKind::kClass:
      return 0;
    case cloth::FileTypeKind::kInterface:
      return 1;
    case cloth::FileTypeKind::kEnum:
      return 2;
    case cloth::FileTypeKind::kStruct:
      return 3;
    case cloth::FileTypeKind::kError:
      return 4;
  }
  return -1;
}

void write_nominal(const cloth::NominalIdentity& value) {
  std::cout << "N(";
  write_bytes(value.package.name);
  std::cout << ';';
  write_bytes(value.package.version);
  std::cout << ';';
  write_bytes(value.source_package);
  std::cout << ';';
  write_bytes(value.name);
  std::cout << ';' << nominal_kind_code(value.kind) << ')';
}

void write_type(cloth::TypeId id, const cloth::SemanticModel& semantics) {
  const cloth::SemanticType& value = semantics.type(id);
  if (value.kind == cloth::TypeKind::kArray) {
    std::cout << "A(";
    write_type(*value.element_type, semantics);
    std::cout << ')';
    return;
  }
  if (value.kind == cloth::TypeKind::kNullable) {
    std::cout << "Q(";
    write_type(*value.element_type, semantics);
    std::cout << ')';
    return;
  }
  if (value.file) {
    write_nominal(semantics.file(*value.file).identity);
    return;
  }
  std::cout << "C(";
  write_bytes(value.name);
  std::cout << ')';
}

void write_symbol(int kind, const cloth::SemanticSymbol& value,
                  const cloth::SemanticModel& semantics) {
  std::cout << "M|" << kind << '|';
  write_bytes(value.name);
  std::cout << '|';
  write_type(value.type, semantics);
  std::cout << '|' << visibility_code(value.visibility) << '|';
  write_flag(value.is_valid);
  std::cout << '|';
  write_flag(value.is_final);
  std::cout << '|';
  write_flag(value.is_static);
  std::cout << '|';
  write_flag(value.is_override);
  std::cout << '|';
  write_flag(value.is_abstract);
  std::cout << '|' << value.parameter_types.size();
  for (cloth::TypeId parameter : value.parameter_types) {
    std::cout << '|';
    write_type(parameter, semantics);
  }
  std::cout << '\n';
}

struct DiagnosticRecord {
  int kind;
  cloth::SourceLocation location;
  cloth::SourceLocation related;
};

int diagnostic_kind(std::string_view message) {
  if (message.find("unknown type 'String'") != std::string_view::npos) {
    return 1;
  }
  if (message.find("unknown type") != std::string_view::npos) return 0;
  if (message.find("only valid as a function return type") !=
      std::string_view::npos) {
    return 2;
  }
  if (message.find("cannot be an array element type") !=
      std::string_view::npos) {
    return 3;
  }
  if (message.find("cannot be nullable") != std::string_view::npos) return 4;
  return -1;
}

bool write_diagnostics(const cloth::DiagnosticEngine& diagnostics) {
  std::vector<DiagnosticRecord> records;
  const std::span<const cloth::Diagnostic> values = diagnostics.diagnostics();
  for (std::size_t index = 0; index < values.size(); ++index) {
    const cloth::Diagnostic& diagnostic = values[index];
    if (diagnostic.severity != cloth::DiagnosticSeverity::kError) continue;
    const int kind = diagnostic_kind(diagnostic.message);
    if (kind < 0) return false;
    cloth::SourceLocation related{
        .file = {}, .byte_offset = 0, .line = 0, .column = 0};
    if (index + 1 < values.size() &&
        values[index + 1].severity == cloth::DiagnosticSeverity::kNote) {
      related = values[index + 1].range.begin;
    }
    records.push_back({kind, diagnostic.range.begin, related});
  }
  std::ranges::sort(records, {}, &DiagnosticRecord::kind);
  for (const DiagnosticRecord& record : records) {
    std::cout << "D|" << record.kind << '|' << record.location.line << '|'
              << record.location.column << '|' << record.related.line << '|'
              << record.related.column << '\n';
  }
  return !records.empty();
}

int write_records(const std::filesystem::path& path) {
  auto loaded = cloth::SourceFile::load(path);
  if (!loaded) {
    std::cerr << path.string() << ": " << loaded.error().message << '\n';
    return 1;
  }

  cloth::Compilation compilation;
  compilation.add_source(std::move(*loaded));
  cloth::DiagnosticEngine diagnostics;
  cloth::FrontendResult result = compilation.analyze_frontend(diagnostics);
  if (diagnostics.has_errors()) {
    if (write_diagnostics(diagnostics)) return 0;
    std::cerr << "semantic parity input has unsupported diagnostics\n";
    return 1;
  }
  if (!result.is_valid || result.semantics.files().size() != 1) {
    std::cerr << "semantic parity input is not a valid standalone file\n";
    return 1;
  }

  const cloth::SemanticModel& semantics = result.semantics;
  const cloth::FileSemantics& file = semantics.file(cloth::FileId{0});
  const cloth::SemanticSymbol& file_symbol = semantics.symbol(file.symbol);
  std::cout << "F|" << file_kind_code(file.kind) << '|'
            << visibility_code(file_symbol.visibility) << '|';
  write_flag(file.is_valid);
  std::cout << '|';
  write_flag(file.is_abstract);
  std::cout << '|';
  write_flag(file.is_sealed);
  std::cout << '|';
  write_nominal(file.identity);
  std::cout << '\n';

  for (const cloth::MemberReference& member : file.member_order) {
    if (member.kind == cloth::DeclarationKind::kField) {
      write_symbol(0, semantics.symbol(file.fields[member.index]), semantics);
    } else if (member.kind == cloth::DeclarationKind::kFunction) {
      write_symbol(1, semantics.symbol(file.functions[member.index]),
                   semantics);
    } else {
      write_symbol(2, semantics.symbol(file.constructors[member.index]),
                   semantics);
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) return write_records(argv[1]);
  std::cerr << "usage: cloth_semantic_parity_oracle <path>\n";
  return 2;
}
