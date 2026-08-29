#include "cloth/mir/mir_printer.h"

#include "cloth/ast/ast.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
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
  } else if (const auto* return_terminator =
                 std::get_if<MirReturnTerminator>(&terminator.data)) {
    output << "return";
    if (return_terminator->value) {
      output << " %" << return_terminator->value->value;
    }
  } else {
    output << "unreachable";
  }
}

void print_body(const MirBody& body, std::ostream& output) {
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBasicBlock& block = body.blocks[index];
    output << "|  |- bb" << index;
    if (!block.is_reachable) {
      output << " [dead]";
    }
    output << ": " << block.instructions.size() << " instruction(s), ";
    print_terminator(block.terminator, output);
    output << '\n';
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
  output << '\n';
  print_body(callable.body, output);
}

}  // namespace

void print_mir_summary(const MirModule& mir, const SemanticModel& semantics,
                       std::ostream& output) {
  for (const MirFileClass& file : mir.files) {
    output << "FileClass " << semantics.symbol(file.symbol).name;
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
    output << '\n';
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
          if (field.initializer) {
            output << " [initializer]\n";
            print_body(*field.initializer, output);
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
