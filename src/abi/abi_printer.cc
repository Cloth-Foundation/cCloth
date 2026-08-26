#include "cloth/abi/abi_printer.h"

#include "cloth/abi/abi.h"
#include "cloth/ast/ast.h"
#include "cloth/sema/semantic_model.h"

#include <cstddef>
#include <ostream>
#include <string_view>

namespace cloth {
namespace {

std::string_view linkage_name(AbiLinkage linkage) noexcept {
  return linkage == AbiLinkage::kExternal ? "external" : "internal";
}

void print_callable(const AbiCallable& callable, const SemanticModel& semantics,
                    std::ostream& output) {
  const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
  output << "|- "
         << (callable.kind == AbiCallableKind::kConstructor ? "Constructor "
                                                            : "Function ")
         << symbol.name << " [" << linkage_name(callable.linkage) << "]\n";
  output << "|  |- ABI " << callable.mangled_name << '(';
  for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    const AbiParameter& parameter = callable.parameters[index];
    if (parameter.kind == AbiParameterKind::kReceiver) {
      output << "receiver:";
    }
    output << semantics.type(parameter.type).name;
  }
  output << ") -> " << semantics.type(callable.return_type).name << '\n';
}

}  // namespace

void print_abi_summary(const AbiModule& abi, const SemanticModel& semantics,
                       std::ostream& output) {
  output << "Target " << abi.target.target_name << " ["
         << (abi.target.endianness == Endianness::kLittle ? "little" : "big")
         << " endian, pointer " << abi.target.pointer.size << '/'
         << abi.target.pointer.alignment << "]\n";
  for (const AbiFileClass& file : abi.files) {
    const SemanticSymbol& class_symbol = semantics.symbol(file.symbol);
    output << "FileClass " << class_symbol.name << " [size " << file.layout.size
           << ", align " << file.layout.alignment << ", header "
           << file.layout.header_size << "]\n";
    for (const MemberReference& member : file.member_order) {
      switch (member.kind) {
        case DeclarationKind::kField: {
          const AbiFieldLayout& field = file.layout.fields.at(member.index);
          const SemanticSymbol& symbol = semantics.symbol(field.symbol);
          const AbiTypeLayout& type = abi.types.at(field.type.value);
          output << "|- Field " << symbol.name << ": "
                 << semantics.type(field.type).name << " [offset "
                 << field.offset << ", size " << type.storage.size << ", align "
                 << type.storage.alignment << "]\n";
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
