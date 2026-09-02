// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_printer.h"

#include "cloth/abi/abi.h"
#include "cloth/ast/ast.h"
#include "cloth/sema/semantic_model.h"

#include <algorithm>
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
         << symbol.name << " [" << linkage_name(callable.linkage);
  if (symbol.is_static) {
    output << ", static";
  }
  if (symbol.virtual_slot) {
    output << ", virtual slot " << *symbol.virtual_slot;
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
  output << "]\n";
  output << "|  |- ABI " << callable.mangled_name << '(';
  for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    const AbiParameter& parameter = callable.parameters[index];
    if (parameter.kind == AbiParameterKind::kResult) {
      output << "result:";
    }
    if (parameter.kind == AbiParameterKind::kReceiver) {
      output << "receiver:";
    }
    output << semantics.type(parameter.type).name;
    if (parameter.passing != AbiPassingMode::kDirect) output << "*";
  }
  output << ") -> " << semantics.type(callable.return_type).name << '\n';
  if (!callable.initializer_mangled_name.empty()) {
    output << "|  |- InitializerABI " << callable.initializer_mangled_name
           << "(receiver";
    for (const AbiParameter& parameter : callable.parameters) {
      output << ", " << semantics.type(parameter.type).name;
    }
    output << ") -> void\n";
  }
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
    if (file.kind == FileTypeKind::kEnum) {
      const AbiTypeLayout& type = abi.types.at(class_symbol.type.value);
      output << "Enum " << class_symbol.name << " [scalar uint32, size "
             << type.storage.size << ", align " << type.storage.alignment
             << ", references none, cases "
             << semantics.file(file.file).enum_cases.size() << "]\n";
      continue;
    }
    if (file.kind == FileTypeKind::kInterface) {
      const FileSemantics& interface_file = semantics.file(file.file);
      output << "Interface " << class_symbol.name << " [id "
             << interface_file.interface_id.value_or(0) << ", contracts "
             << interface_file.interface_functions.size() << "]\n";
      for (const MemberReference& member : file.member_order) {
        if (member.kind == DeclarationKind::kFunction) {
          print_callable(file.functions.at(member.index), semantics, output);
        }
      }
      continue;
    }
    output << (file.kind == FileTypeKind::kStruct ? "Struct " : "FileClass ")
           << class_symbol.name;
    if (file.base_file) {
      output << " : "
             << semantics.symbol(semantics.file(*file.base_file).symbol).name;
    }
    if (file.type_descriptor && !file.type_descriptor->interfaces.empty()) {
      output << " is ";
      for (std::size_t index = 0;
           index < file.type_descriptor->interfaces.size(); ++index) {
        if (index != 0) {
          output << ", ";
        }
        const FileId interface_file =
            file.type_descriptor->interfaces[index].interface_file;
        output << semantics.symbol(semantics.file(interface_file).symbol).name;
      }
    }
    output << " [size " << file.layout.size << ", align "
           << file.layout.alignment << ", header " << file.layout.header_size
           << ", references ";
    const auto& references =
        file.kind == FileTypeKind::kStruct
            ? abi.types.at(class_symbol.type.value).reference_offsets
            : file.type_descriptor->reference_offsets;
    if (references.empty()) {
      output << "none";
    } else {
      for (std::size_t index = 0; index < references.size(); ++index) {
        if (index != 0) {
          output << ',';
        }
        output << references[index];
      }
    }
    output << ", virtuals "
           << (file.type_descriptor
                   ? file.type_descriptor->virtual_functions.size()
                   : 0)
           << (semantics.file(file.file).is_abstract ? ", abstract" : "")
           << (semantics.file(file.file).is_sealed ? ", sealed" : "") << "]\n";
    for (const MemberReference& member : file.member_order) {
      switch (member.kind) {
        case DeclarationKind::kField: {
          const SymbolId symbol_id =
              semantics.file(file.file).fields.at(member.index);
          const SemanticSymbol& symbol = semantics.symbol(symbol_id);
          if (symbol.is_static) {
            const auto field = std::find_if(
                file.static_fields.begin(), file.static_fields.end(),
                [symbol_id](const AbiStaticField& candidate) {
                  return candidate.symbol == symbol_id;
                });
            if (field != file.static_fields.end()) {
              output << "|- StaticField " << symbol.name << ": "
                     << semantics.type(field->type).name << " ["
                     << linkage_name(field->linkage) << ", ABI "
                     << field->mangled_name << "]\n";
            }
            break;
          }
          const auto field =
              std::find_if(file.layout.fields.begin(), file.layout.fields.end(),
                           [symbol_id](const AbiFieldLayout& candidate) {
                             return candidate.symbol == symbol_id;
                           });
          if (field == file.layout.fields.end()) {
            break;
          }
          const AbiTypeLayout& type = abi.types.at(field->type.value);
          output << "|- Field " << symbol.name << ": "
                 << semantics.type(field->type).name << " [offset "
                 << field->offset << ", size " << type.storage.size
                 << ", align " << type.storage.alignment << "]\n";
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
