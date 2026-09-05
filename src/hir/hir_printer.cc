// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/hir/hir_printer.h"

#include "cloth/ast/ast.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
#include <ostream>

namespace cloth {
namespace {

void print_parameters(const SemanticSymbol& symbol,
                      const SemanticModel& semantics, std::ostream& output) {
  output << '(';
  for (std::size_t index = 0; index < symbol.parameter_types.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << semantics.type(symbol.parameter_types[index]).name;
  }
  output << ')';
}

void print_throws(const SemanticSymbol& symbol, const SemanticModel& semantics,
                  std::ostream& output) {
  if (symbol.thrown_types.empty()) {
    return;
  }
  output << " throws ";
  for (std::size_t index = 0; index < symbol.thrown_types.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << semantics.type(symbol.thrown_types[index]).name;
  }
}

}  // namespace

void print_hir_summary(const HirModule& hir, const SemanticModel& semantics,
                       std::ostream& output) {
  for (const HirFileClass& file : hir.files) {
    const SemanticSymbol& class_symbol = semantics.symbol(file.symbol);
    const FileSemantics& file_semantics = semantics.file(file.file);
    output << (file_semantics.kind == FileTypeKind::kStruct      ? "Struct "
               : file_semantics.kind == FileTypeKind::kEnum      ? "Enum "
               : file_semantics.kind == FileTypeKind::kInterface ? "Interface "
               : file_semantics.kind == FileTypeKind::kError     ? "Error "
                                                                 : "FileClass ")
           << class_symbol.name << " : "
           << semantics.type(class_symbol.type).name << " ["
           << visibility_name(class_symbol.visibility);
    if (file.base_file) {
      output << ", base "
             << semantics.symbol(semantics.file(*file.base_file).symbol).name;
    }
    if (semantics.file(file.file).is_abstract) {
      output << ", abstract";
    }
    if (semantics.file(file.file).is_sealed) {
      output << ", sealed";
    }
    if (!file_semantics.interfaces.empty()) {
      output << ", interfaces " << file_semantics.interfaces.size();
    }
    output << "]\n";
    for (const SymbolId case_id : file_semantics.enum_cases) {
      output << "|- Case " << semantics.symbol(case_id).name << " [public]\n";
    }
    for (const MemberReference& reference : file.member_order) {
      switch (reference.kind) {
        case DeclarationKind::kField: {
          const SemanticSymbol& symbol =
              semantics.symbol(file.fields.at(reference.index).symbol);
          output << "|- Field " << symbol.name << ": ";
          if (symbol.is_static) {
            output << "static ";
          }
          if (symbol.is_final) {
            output << "final ";
          }
          output << semantics.type(symbol.type).name << " ["
                 << visibility_name(symbol.visibility) << "]\n";
          break;
        }
        case DeclarationKind::kFunction: {
          const SemanticSymbol& symbol =
              semantics.symbol(file.functions.at(reference.index).symbol);
          output << "|- Function " << symbol.name;
          print_parameters(symbol, semantics, output);
          print_throws(symbol, semantics, output);
          output << ": " << semantics.type(symbol.type).name << " ["
                 << visibility_name(symbol.visibility);
          if (symbol.is_static) {
            output << ", static";
          }
          if (file.functions.at(reference.index).struct_receiver ==
              StructReceiverMode::kReadOnlyValue) {
            output << ", read-only value receiver";
          }
          if (symbol.is_override) {
            output << ", override";
          }
          if (symbol.is_abstract) {
            output << ", abstract";
          }
          if (symbol.is_final) {
            output << ", final";
          }
          if (symbol.virtual_slot) {
            output << ", virtual slot " << *symbol.virtual_slot;
          }
          output << "]\n";
          break;
        }
        case DeclarationKind::kConstructor: {
          const SemanticSymbol& symbol =
              semantics.symbol(file.constructors.at(reference.index).symbol);
          output << "|- Constructor " << symbol.name;
          print_parameters(symbol, semantics, output);
          print_throws(symbol, semantics, output);
          const HirCallable& constructor =
              file.constructors.at(reference.index);
          if (constructor.initializer) {
            output
                << " : "
                << semantics.symbol(constructor.initializer->constructor).name
                << '(' << constructor.initializer->arguments.size()
                << " argument(s))";
          }
          output << " -> " << semantics.type(symbol.type).name << " ["
                 << visibility_name(symbol.visibility) << "]\n";
          break;
        }
        case DeclarationKind::kNestedType:
          break;
      }
    }
  }
}

}  // namespace cloth
