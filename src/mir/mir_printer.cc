// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/mir/mir_printer.h"

#include "cloth/ast/ast.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
#include <iomanip>
#include <ostream>
#include <variant>

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

void print_terminator(const MirTerminator& terminator, std::ostream& output) {
  if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
    output << "jump bb" << jump->target.value;
  } else if (const auto* branch =
                 std::get_if<MirBranchTerminator>(&terminator.data)) {
    output << "branch %" << branch->condition.value << ", bb"
           << branch->then_block.value << ", bb" << branch->else_block.value;
  } else if (const auto* selection =
                 std::get_if<MirSwitchTerminator>(&terminator.data)) {
    output << "switch %" << selection->selector.value << " type#"
           << selection->selector_type.value;
    for (const auto& entry : selection->cases)
      output << ", " << entry.value.bits << ": bb" << entry.target.value;
    output << ", default: bb" << selection->default_block.value;
    if (selection->invalid_block)
      output << ", invalid: bb" << selection->invalid_block->value;
  } else if (std::holds_alternative<MirTrapTerminator>(terminator.data)) {
    output << "trap";
  } else if (const auto* return_terminator =
                 std::get_if<MirReturnTerminator>(&terminator.data)) {
    output << "return";
    if (return_terminator->value) {
      output << " %" << return_terminator->value->value;
    }
  } else if (const auto* error =
                 std::get_if<MirErrorTerminator>(&terminator.data)) {
    output << "error %" << error->error.value;
  } else {
    output << "unreachable";
  }
}

void print_body(const MirBody& body, const SemanticModel& semantics,
                std::ostream& output) {
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBasicBlock& block = body.blocks[index];
    output << "|  |- bb" << index;
    if (!block.is_reachable) {
      output << " [dead]";
    }
    output << ": " << block.instructions.size() << " instruction(s), ";
    print_terminator(block.terminator, output);
    output << '\n';
    for (const MirInstruction& instruction : block.instructions) {
      const auto* constant =
          std::get_if<MirScalarConstantInstruction>(&instruction.data);
      if (constant == nullptr || !instruction.result) {
        continue;
      }
      output << "|  |  |- %" << instruction.result->value << " = constant "
             << "type#" << constant->value.type.value;
      if (constant->value.type.value < semantics.types().size()) {
        output << '(' << semantics.type(constant->value.type).name << ')';
      }
      const auto flags = output.flags();
      const char fill = output.fill();
      output << " 0x" << std::hex << std::setfill('0') << std::setw(16)
             << constant->value.bits << '\n';
      output.flags(flags);
      output.fill(fill);
    }
  }
}

void print_callable(const MirCallable& callable, const SemanticModel& semantics,
                    std::ostream& output) {
  const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
  output << "|- "
         << (symbol.kind == SymbolKind::kConstructor ? "Constructor "
                                                     : "Function ")
         << symbol.name;
  print_parameters(symbol, semantics, output);
  if (symbol.kind == SymbolKind::kFunction) {
    output << ": " << semantics.type(symbol.type).name;
  }
  if (symbol.is_static) {
    output << " [static]";
  }
  if (symbol.virtual_slot) {
    output << " [virtual slot " << *symbol.virtual_slot << ']';
  }
  if (symbol.is_override) {
    output << " [override]";
  }
  if (symbol.is_abstract) {
    output << " [abstract]";
  }
  if (symbol.is_final) {
    output << " [final]";
  }
  if (callable.struct_receiver == StructReceiverMode::kReadOnlyValue)
    output << " [read-only value receiver]";
  if (callable.struct_receiver == StructReceiverMode::kConstruction)
    output << " [construction receiver]";
  output << '\n';
  print_body(callable.body, semantics, output);
}

}  // namespace

void print_mir_summary(const MirModule& mir, const SemanticModel& semantics,
                       std::ostream& output) {
  for (const MirFileClass& file : mir.files) {
    const FileSemantics& file_semantics = semantics.file(file.file);
    output << (file_semantics.kind == FileTypeKind::kEnum        ? "Enum "
               : file_semantics.kind == FileTypeKind::kError     ? "Error "
               : file_semantics.kind == FileTypeKind::kStruct    ? "Struct "
               : file_semantics.kind == FileTypeKind::kInterface ? "Interface "
                                                                 : "FileClass ")
           << semantics.symbol(file.symbol).name;
    if (file.base_file) {
      output << " : "
             << semantics.symbol(semantics.file(*file.base_file).symbol).name;
    }
    if (semantics.file(file.file).is_abstract) {
      output << " [abstract]";
    }
    if (semantics.file(file.file).is_sealed) {
      output << " [sealed]";
    }
    if (!file_semantics.interfaces.empty()) {
      output << " [interfaces " << file_semantics.interfaces.size() << ']';
    }
    output << '\n';
    for (const SymbolId case_id : file_semantics.enum_cases) {
      const SemanticSymbol& item = semantics.symbol(case_id);
      output << "|- Case " << item.name << " [tag " << *item.enum_tag << "]\n";
    }
    for (const MemberReference& member : file.member_order) {
      switch (member.kind) {
        case DeclarationKind::kField: {
          const MirField& field = file.fields.at(member.index);
          const SemanticSymbol& symbol = semantics.symbol(field.symbol);
          output << "|- Field " << symbol.name << ": ";
          if (symbol.is_static) {
            output << "static ";
          }
          if (symbol.is_final) {
            output << "final ";
          }
          output << semantics.type(symbol.type).name;
          if (field.static_constant) {
            output << " [constant bits=" << field.static_constant->bits << "]";
          }
          if (field.initializer) {
            output << " [initializer]\n";
            print_body(*field.initializer, semantics, output);
          } else {
            output << '\n';
          }
          break;
        }
        case DeclarationKind::kFunction:
          print_callable(file.functions.at(member.index), semantics, output);
          break;
        case DeclarationKind::kConstructor:
          print_callable(file.constructors.at(member.index), semantics, output);
          break;
        case DeclarationKind::kNestedType:
          break;
      }
    }
  }
}

}  // namespace cloth
