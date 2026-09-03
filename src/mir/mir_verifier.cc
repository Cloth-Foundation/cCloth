// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/mir/mir_verifier.h"

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

class MirVerifier {
 public:
  MirVerifier(const MirModule& mir, const SemanticModel& semantics,
              DiagnosticEngine& diagnostics)
      : mir_(mir), semantics_(semantics), diagnostics_(diagnostics) {}

  bool run() {
    if (mir_.files.size() != semantics_.files().size()) {
      report(fallback_range(), "file count does not match the semantic model");
    }
    for (std::size_t index = 0; index < mir_.files.size(); ++index) {
      verify_file(mir_.files[index], index);
    }
    return is_valid_;
  }

 private:
  bool is_struct_type(std::optional<TypeId> type) const {
    return type && type->value < semantics_.types().size() &&
           semantics_.type(*type).kind == TypeKind::kStruct;
  }

  StructReceiverMode receiver_mode(const SemanticSymbol& symbol) const {
    if (!symbol.file ||
        semantics_.file(*symbol.file).kind != FileTypeKind::kStruct)
      return StructReceiverMode::kNone;
    if (symbol.kind == SymbolKind::kConstructor)
      return StructReceiverMode::kConstruction;
    return symbol.is_static ? StructReceiverMode::kNone
                            : StructReceiverMode::kReadOnlyValue;
  }

  bool is_enum_type(std::optional<TypeId> type) const {
    return type && type->value < semantics_.types().size() &&
           semantics_.type(*type).kind == TypeKind::kEnum;
  }

  void verify_file(const MirFileClass& file, std::size_t file_index) {
    current_file_ = file.file;
    const SourceRange range = symbol_range(file.symbol);
    if (file.file.value >= semantics_.files().size()) {
      report(range, "file class has an unknown FileId");
    } else {
      const FileSemantics& semantic_file = semantics_.file(file.file);
      if (file.file != FileId{file_index}) {
        report(range, "file class order does not match its FileId");
      }
      if (file.symbol != semantic_file.symbol) {
        report(range, "file class symbol does not match semantics");
      }
      if (file.base_file != semantic_file.base_file) {
        report(range, "file class base does not match semantics");
      }
      if (file.base_file &&
          (file.base_file->value >= semantics_.files().size() ||
           *file.base_file == file.file)) {
        report(range, "file class has an invalid base FileId");
      }
      if (file.fields.size() != semantic_file.fields.size() ||
          file.functions.size() != semantic_file.functions.size() ||
          file.constructors.size() != semantic_file.constructors.size()) {
        report(range, "file member counts do not match semantics");
      }
    }
    verify_symbol(file.symbol, range);
    current_parameters_.clear();
    std::set<std::size_t> initialized_fields;
    for (const MirField& field : file.fields) {
      verify_symbol(field.symbol, range);
      if (file.is_imported_declaration && field.initializer) {
        report(range, "imported field declaration contains an initializer");
      }
      if (field.initializer) {
        verify_body(*field.initializer,
                    symbol_type(field.symbol, semantics_.error_type()), true);
        verify_struct_initialization(file, *field.initializer,
                                     initialized_fields, false);
        initialized_fields.insert(field.symbol.value);
      }
    }
    for (const MirCallable& function : file.functions) {
      if (file.is_imported_declaration) {
        verify_imported_callable(function, SymbolKind::kFunction);
      } else {
        verify_callable(function, SymbolKind::kFunction);
      }
    }
    for (const MirCallable& constructor : file.constructors) {
      if (file.is_imported_declaration) {
        verify_imported_callable(constructor, SymbolKind::kConstructor);
      } else {
        verify_callable(constructor, SymbolKind::kConstructor);
        verify_struct_initialization(file, constructor.body, {}, true);
      }
    }
    for (const MemberReference& member : file.member_order) {
      switch (member.kind) {
        case DeclarationKind::kField:
          if (member.index >= file.fields.size()) {
            report(range, "member order references an unknown field");
          }
          break;
        case DeclarationKind::kFunction:
          if (member.index >= file.functions.size()) {
            report(range, "member order references an unknown function");
          }
          break;
        case DeclarationKind::kConstructor:
          if (member.index >= file.constructors.size()) {
            report(range, "member order references an unknown constructor");
          }
          break;
        case DeclarationKind::kNestedType:
          break;
      }
    }
  }

  void verify_imported_callable(const MirCallable& callable,
                                SymbolKind expected_kind) {
    const SourceRange range = symbol_range(callable.symbol);
    verify_symbol(callable.symbol, range);
    if (!callable.body.blocks.empty() || callable.body.value_count != 0) {
      report(range, "imported callable declaration contains a MIR body");
    }
    if (callable.symbol.value >= semantics_.symbols().size()) {
      return;
    }
    const SemanticSymbol& symbol = semantics_.symbol(callable.symbol);
    if (callable.struct_receiver != receiver_mode(symbol)) {
      report(range, "imported callable has an invalid struct receiver mode");
    }
    if (symbol.kind != expected_kind) {
      report(range, "imported callable has the wrong symbol kind");
    }
    if (callable.parameters != symbol.parameter_symbols ||
        callable.parameters.size() != symbol.parameter_types.size()) {
      report(range,
             "imported callable parameters do not match its declaration");
    }
    for (const SymbolId parameter : callable.parameters) {
      verify_symbol(parameter, range);
      if (parameter.value < semantics_.symbols().size() &&
          semantics_.symbol(parameter).kind != SymbolKind::kParameter) {
        report(range, "imported callable parameter has the wrong symbol kind");
      }
    }
  }

  void verify_callable(const MirCallable& callable, SymbolKind expected_kind) {
    current_parameters_.clear();
    for (const auto parameter : callable.parameters) {
      current_parameters_.insert(parameter.value);
    }
    const SourceRange range = symbol_range(callable.symbol);
    verify_symbol(callable.symbol, range);
    TypeId return_type = semantics_.error_type();
    bool is_abstract = false;
    if (callable.symbol.value < semantics_.symbols().size()) {
      const SemanticSymbol& symbol = semantics_.symbol(callable.symbol);
      if (symbol.kind != expected_kind) {
        report(range, "callable has the wrong symbol kind");
      }
      if (callable.parameters != symbol.parameter_symbols) {
        report(range, "callable parameter symbols do not match semantics");
      }
      if (callable.parameters.size() != symbol.parameter_types.size()) {
        report(range, "callable parameter count does not match its signature");
      }
      return_type = expected_kind == SymbolKind::kConstructor
                        ? semantics_.void_type()
                        : symbol.type;
      is_abstract = symbol.is_abstract;
      if (callable.struct_receiver != receiver_mode(symbol)) {
        report(range, "callable has an invalid struct receiver mode");
      }
      verify_constructor_initialization(callable, symbol, expected_kind, range);
    }
    for (const SymbolId parameter : callable.parameters) {
      verify_symbol(parameter, range);
      if (parameter.value < semantics_.symbols().size() &&
          semantics_.symbol(parameter).kind != SymbolKind::kParameter) {
        report(range, "callable parameter has the wrong symbol kind");
      }
    }
    verify_body(callable.body, return_type,
                expected_kind == SymbolKind::kConstructor,
                expected_kind == SymbolKind::kConstructor);
    if (is_abstract && (callable.body.blocks.size() != 1 ||
                        !callable.body.blocks[0].instructions.empty() ||
                        !std::holds_alternative<MirUnreachableTerminator>(
                            callable.body.blocks[0].terminator.data))) {
      report(range, "abstract function body is not an unreachable stub");
    }
  }

  void verify_constructor_initialization(const MirCallable& callable,
                                         const SemanticSymbol& symbol,
                                         SymbolKind expected_kind,
                                         SourceRange range) {
    std::size_t field_initializers = 0;
    std::size_t base_calls = 0;
    std::optional<std::pair<std::size_t, std::size_t>> field_position;
    std::optional<std::pair<std::size_t, std::size_t>> base_position;
    for (std::size_t block_index = 0; block_index < callable.body.blocks.size();
         ++block_index) {
      const MirBasicBlock& block = callable.body.blocks[block_index];
      for (std::size_t instruction_index = 0;
           instruction_index < block.instructions.size(); ++instruction_index) {
        const MirInstruction& instruction =
            block.instructions[instruction_index];
        if (std::holds_alternative<MirInitializeFieldsInstruction>(
                instruction.data)) {
          ++field_initializers;
          field_position = {block_index, instruction_index};
        }
        const auto* call = std::get_if<MirCallInstruction>(&instruction.data);
        if (call != nullptr && call->kind == MirCallKind::kBaseConstructor) {
          ++base_calls;
          base_position = {block_index, instruction_index};
          if (symbol.base_constructor != call->callable) {
            report(instruction.range,
                   "base constructor call does not match semantics");
          }
        }
      }
    }

    if (expected_kind != SymbolKind::kConstructor) {
      if (field_initializers != 0 || base_calls != 0) {
        report(range, "non-constructor contains initialization instructions");
      }
      return;
    }
    if (field_initializers != 1) {
      report(range,
             "constructor must contain exactly one field initialization");
    }
    const std::size_t expected_base_calls = symbol.base_constructor ? 1U : 0U;
    if (base_calls != expected_base_calls) {
      report(range, "constructor has the wrong number of base calls");
    }
    if (base_position && field_position &&
        (base_position->first != field_position->first ||
         base_position->second >= field_position->second)) {
      report(range, "base constructor call must precede field initialization");
    }
  }

  // Recheck construction state at the MIR boundary. A storage recipe can name
  // an incomplete field; copying self, reading it, or returning cannot expose
  // uninitialized inline data (including primitive fields).
  void verify_struct_initialization(const MirFileClass& file,
                                    const MirBody& body,
                                    const std::set<std::size_t>& initial_fields,
                                    bool constructor) {
    if (file.file.value >= semantics_.files().size() ||
        semantics_.file(file.file).kind != FileTypeKind::kStruct ||
        body.entry.value >= body.blocks.size())
      return;
    const auto self = semantics_.file(file.file).self_symbol;
    std::map<std::size_t, std::size_t> indices;
    std::vector<bool> defaults;
    std::vector<bool> finals;
    for (const auto& field : file.fields) {
      if (field.symbol.value >= semantics_.symbols().size()) return;
      const auto& symbol = semantics_.symbol(field.symbol);
      if (symbol.is_static) continue;
      indices.emplace(field.symbol.value, defaults.size());
      defaults.push_back(field.initializer.has_value());
      finals.push_back(symbol.is_final);
    }
    struct State {
      std::vector<bool> definite;
      std::vector<bool> possible;
      bool operator==(const State&) const = default;
    };
    State initial{std::vector<bool>(indices.size()),
                  std::vector<bool>(indices.size())};
    for (const auto field : initial_fields) {
      const auto found = indices.find(field);
      if (found != indices.end()) {
        initial.definite[found->second] = true;
        initial.possible[found->second] = true;
      }
    }
    const auto transfer = [&](const MirBasicBlock& block, State state,
                              bool diagnose) {
      const auto read_all = [&](SourceRange range) {
        if (diagnose &&
            std::ranges::find(state.definite, false) != state.definite.end()) {
          report(range, "incomplete struct self is copied or escapes");
        }
      };
      const auto read = [&](SymbolId field, SourceRange range) {
        const auto found = indices.find(field.value);
        if (diagnose && found != indices.end() &&
            !state.definite[found->second]) {
          report(range, "struct field is read before initialization");
        }
      };
      const auto write = [&](std::size_t index, SourceRange range) {
        if (diagnose && finals[index] && state.possible[index]) {
          report(range, "final struct field may be initialized more than once");
        }
        state.definite[index] = true;
        state.possible[index] = true;
      };
      for (const auto& instruction : block.instructions) {
        if (std::holds_alternative<MirInitializeFieldsInstruction>(
                instruction.data)) {
          for (std::size_t index = 0; index < defaults.size(); ++index) {
            if (defaults[index]) write(index, instruction.range);
          }
        } else if (const auto* load = std::get_if<MirLoadStorageInstruction>(
                       &instruction.data);
                   load && load->path.symbol == self) {
          if (load->path.fields.empty())
            read_all(instruction.range);
          else
            read(load->path.fields.front(), instruction.range);
        } else if (const auto* store = std::get_if<MirStoreStorageInstruction>(
                       &instruction.data);
                   store && store->path.symbol == self &&
                   !store->path.fields.empty()) {
          const auto found = indices.find(store->path.fields.front().value);
          if (store->path.fields.size() == 1 && found != indices.end()) {
            write(found->second, instruction.range);
          } else {
            read(store->path.fields.front(), instruction.range);
          }
        } else if (const auto* load = std::get_if<MirLoadSymbolInstruction>(
                       &instruction.data)) {
          if (load->symbol == self)
            read_all(instruction.range);
          else
            read(load->symbol, instruction.range);
        }
      }
      if (constructor &&
          std::holds_alternative<MirReturnTerminator>(block.terminator.data)) {
        read_all(block.terminator.range);
      }
      return state;
    };
    std::vector<std::optional<State>> incoming(body.blocks.size());
    incoming[body.entry.value] = initial;
    std::deque<std::size_t> pending{body.entry.value};
    std::vector<bool> queued(body.blocks.size());
    queued[body.entry.value] = true;
    while (!pending.empty()) {
      const auto index = pending.front();
      pending.pop_front();
      queued[index] = false;
      const auto out = transfer(body.blocks[index], *incoming[index], false);
      const auto propagate = [&](MirBlockId destination) {
        if (destination.value >= incoming.size()) return;
        auto& next = incoming[destination.value];
        State merged = next.value_or(out);
        if (next) {
          for (std::size_t field = 0; field < defaults.size(); ++field) {
            merged.definite[field] =
                merged.definite[field] && out.definite[field];
            merged.possible[field] =
                merged.possible[field] || out.possible[field];
          }
        }
        if (!next || *next != merged) {
          next = std::move(merged);
          if (!queued[destination.value]) {
            pending.push_back(destination.value);
            queued[destination.value] = true;
          }
        }
      };
      for (const auto successor : mir_successors(body.blocks[index].terminator))
        propagate(successor);
    }
    for (std::size_t index = 0; index < body.blocks.size(); ++index) {
      if (incoming[index]) {
        static_cast<void>(transfer(body.blocks[index], *incoming[index], true));
      }
    }
  }

  void verify_body(const MirBody& body, TypeId return_type,
                   bool suppress_self_virtual_dispatch,
                   bool initialization_allowed = false) {
    initialization_allowed_ = initialization_allowed;
    current_locals_.clear();
    for (const auto& block : body.blocks) {
      for (const auto& instruction : block.instructions) {
        if (const auto* local =
                std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
          current_locals_.insert(local->symbol.value);
        }
      }
    }
    if (body.blocks.empty()) {
      report(body.range, "body has no basic blocks");
      return;
    }
    if (body.entry.value >= body.blocks.size()) {
      report(body.range, "body has an unknown entry block");
    }

    std::vector<std::optional<TypeId>> value_types(body.value_count);
    for (const MirBasicBlock& block : body.blocks) {
      for (const MirInstruction& instruction : block.instructions) {
        verify_type(instruction.type, instruction.range);
        if (instruction.result) {
          if (instruction.result->value >= value_types.size()) {
            report(instruction.range,
                   "instruction result exceeds the body value table");
          } else if (value_types[instruction.result->value]) {
            report(instruction.range, "value is defined more than once");
          } else {
            value_types[instruction.result->value] = instruction.type;
          }
        }
      }
    }
    for (std::size_t index = 0; index < value_types.size(); ++index) {
      if (!value_types[index]) {
        report(body.range, "body value table contains an undefined value");
      }
    }

    std::vector<std::set<std::size_t>> predecessors(body.blocks.size());
    for (std::size_t index = 0; index < body.blocks.size(); ++index) {
      const auto add = [&](MirBlockId target) {
        if (target.value < predecessors.size())
          predecessors[target.value].insert(index);
      };
      for (const auto successor : mir_successors(body.blocks[index].terminator))
        add(successor);
    }
    for (std::size_t block_index = 0; block_index < body.blocks.size();
         ++block_index) {
      const MirBasicBlock& block = body.blocks[block_index];
      bool saw_non_phi = false;
      for (const MirInstruction& instruction : block.instructions) {
        if (const auto* phi =
                std::get_if<MirPhiInstruction>(&instruction.data)) {
          std::set<std::size_t> incoming;
          for (const auto& edge : phi->incoming)
            incoming.insert(edge.predecessor.value);
          if (saw_non_phi || incoming != predecessors[block_index] ||
              incoming.size() != phi->incoming.size()) {
            report(
                instruction.range,
                "phi does not cover unique predecessor edges at block entry");
          }
        } else {
          saw_non_phi = true;
        }
        verify_instruction(instruction, body, value_types,
                           suppress_self_virtual_dispatch);
      }
      verify_terminator(block.terminator, body, value_types, return_type);
    }
    verify_reachability(body);
    verify_switch_definitions(body);
  }

  void verify_switch_definitions(const MirBody& body) {
    std::vector<std::optional<MirBlockId>> definitions(body.value_count);
    for (std::size_t index = 0; index < body.blocks.size(); ++index) {
      for (const auto& instruction : body.blocks[index].instructions) {
        if (instruction.result &&
            instruction.result->value < definitions.size())
          definitions[instruction.result->value] = MirBlockId{index};
      }
    }
    for (std::size_t index = 0; index < body.blocks.size(); ++index) {
      const auto& block = body.blocks[index];
      const auto* selection =
          std::get_if<MirSwitchTerminator>(&block.terminator.data);
      if (!selection || selection->selector.value >= definitions.size())
        continue;
      const auto definition = definitions[selection->selector.value];
      if (!definition || definition->value == index) continue;
      // Normally lowering defines the captured selector in this block. For
      // transformed MIR, reject any reachable path that bypasses its
      // definition.
      std::vector<bool> visited(body.blocks.size());
      std::vector<MirBlockId> pending{body.entry};
      while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        if (current.value >= body.blocks.size() || visited[current.value] ||
            current == *definition)
          continue;
        if (current.value == index) {
          report(block.terminator.range,
                 "switch selector definition does not dominate dispatch");
          break;
        }
        visited[current.value] = true;
        for (const auto successor :
             mir_successors(body.blocks[current.value].terminator))
          pending.push_back(successor);
      }
    }
  }

  void verify_reachability(const MirBody& body) {
    if (body.entry.value >= body.blocks.size()) {
      return;
    }
    std::vector<bool> reachable(body.blocks.size(), false);
    std::vector<MirBlockId> worklist{body.entry};
    while (!worklist.empty()) {
      const MirBlockId block_id = worklist.back();
      worklist.pop_back();
      if (reachable[block_id.value]) {
        continue;
      }
      reachable[block_id.value] = true;
      for (const auto successor :
           mir_successors(body.blocks[block_id.value].terminator))
        if (successor.value < body.blocks.size()) worklist.push_back(successor);
    }
    for (std::size_t index = 0; index < body.blocks.size(); ++index) {
      if (body.blocks[index].is_reachable != reachable[index]) {
        report(body.range, "basic-block reachability flag is inconsistent");
      }
    }
  }

  void verify_instruction(const MirInstruction& instruction,
                          const MirBody& body,
                          const std::vector<std::optional<TypeId>>& value_types,
                          bool suppress_self_virtual_dispatch) {
    if (const auto* load =
            std::get_if<MirLoadStorageInstruction>(&instruction.data)) {
      require_result(instruction);
      const auto type =
          verify_storage_path(load->path, value_types, instruction.range, false,
                              suppress_self_virtual_dispatch);
      if (type && *type != instruction.type)
        report(instruction.range,
               "storage load has an incompatible result type");
    } else if (const auto* store =
                   std::get_if<MirStoreStorageInstruction>(&instruction.data)) {
      require_no_result(instruction);
      verify_value(store->value, value_types, instruction.range);
      const auto type =
          verify_storage_path(store->path, value_types, instruction.range, true,
                              suppress_self_virtual_dispatch);
      if (type)
        verify_value_type(store->value, *type, value_types, instruction.range);
    } else if (const auto* load =
                   std::get_if<MirLoadSymbolInstruction>(&instruction.data)) {
      verify_symbol(load->symbol, instruction.range);
      require_result(instruction);
      verify_symbol_type(load->symbol, instruction.type, instruction.range);
      if (load->symbol.value < semantics_.symbols().size()) {
        const SemanticSymbol& symbol = semantics_.symbol(load->symbol);
        if (symbol.kind == SymbolKind::kEnumCase ||
            symbol.kind == SymbolKind::kEnum) {
          report(instruction.range,
                 "enum declaration was lowered as a storage load");
        }
        if (symbol.kind == SymbolKind::kField && !symbol.is_static) {
          report(instruction.range,
                 "instance field was lowered as a symbol load");
        }
      }
    } else if (const auto* declaration =
                   std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
      verify_symbol(declaration->symbol, instruction.range);
      verify_optional_value(declaration->initializer, value_types,
                            instruction.range);
      require_no_result(instruction);
      if (declaration->initializer) {
        verify_value_type(
            *declaration->initializer,
            symbol_type(declaration->symbol, semantics_.error_type()),
            value_types, instruction.range);
      } else {
        const TypeId type =
            symbol_type(declaration->symbol, semantics_.error_type());
        if (type.value < semantics_.types().size() &&
            (semantics_.type(type).kind == TypeKind::kEnum ||
             semantics_.type(type).kind == TypeKind::kStruct)) {
          report(instruction.range, "nominal value local has no initializer");
        }
      }
    } else if (const auto* store =
                   std::get_if<MirStoreSymbolInstruction>(&instruction.data)) {
      verify_symbol(store->symbol, instruction.range);
      verify_value(store->value, value_types, instruction.range);
      require_no_result(instruction);
      verify_value_type(store->value,
                        symbol_type(store->symbol, semantics_.error_type()),
                        value_types, instruction.range);
    } else if (const auto* load =
                   std::get_if<MirLoadMemberInstruction>(&instruction.data)) {
      verify_value(load->object, value_types, instruction.range);
      verify_symbol(load->member, instruction.range);
      require_result(instruction);
      verify_symbol_type(load->member, instruction.type, instruction.range);
      if (load->member.value < semantics_.symbols().size()) {
        const SemanticSymbol& symbol = semantics_.symbol(load->member);
        if (symbol.kind != SymbolKind::kField) {
          report(instruction.range, "member load does not reference a field");
        } else if (symbol.is_static) {
          report(instruction.range,
                 "static field was lowered as a member load");
        }
      }
      verify_member_receiver(load->object, load->member, value_types,
                             instruction.range);
    } else if (const auto* store =
                   std::get_if<MirStoreMemberInstruction>(&instruction.data)) {
      verify_value(store->object, value_types, instruction.range);
      verify_symbol(store->member, instruction.range);
      verify_value(store->value, value_types, instruction.range);
      require_no_result(instruction);
      verify_value_type(store->value,
                        symbol_type(store->member, semantics_.error_type()),
                        value_types, instruction.range);
      if (store->member.value < semantics_.symbols().size()) {
        const SemanticSymbol& symbol = semantics_.symbol(store->member);
        if (symbol.kind != SymbolKind::kField) {
          report(instruction.range, "member store does not reference a field");
        } else if (symbol.is_static) {
          report(instruction.range,
                 "static field was lowered as a member store");
        }
      }
      if (is_struct_type(known_value_type(store->object, value_types)))
        report(instruction.range,
               "member store targets a struct value instead of storage");
      verify_member_receiver(store->object, store->member, value_types,
                             instruction.range);
    } else if (const auto* array =
                   std::get_if<MirArrayLiteralInstruction>(&instruction.data)) {
      verify_type(array->element_type, instruction.range);
      require_result(instruction);
      if (instruction.type.value < semantics_.types().size() &&
          instruction.type != semantics_.error_type() &&
          array->element_type != semantics_.error_type()) {
        const SemanticType& type = semantics_.type(instruction.type);
        if (type.kind != TypeKind::kArray ||
            type.element_type != array->element_type) {
          report(instruction.range,
                 "array literal result does not match its element type");
        }
      }
      for (const MirValueId element : array->elements) {
        verify_value(element, value_types, instruction.range);
        verify_value_type(element, array->element_type, value_types,
                          instruction.range);
      }
    } else if (const auto* load =
                   std::get_if<MirArrayLoadInstruction>(&instruction.data)) {
      verify_value(load->array, value_types, instruction.range);
      verify_value(load->index, value_types, instruction.range);
      verify_value_type(load->index, *semantics_.find_type("int32"),
                        value_types, instruction.range);
      require_result(instruction);
      const std::optional<TypeId> element =
          array_element_type(load->array, value_types, instruction.range);
      if (element && instruction.type != *element &&
          instruction.type != semantics_.error_type()) {
        report(instruction.range,
               "array load result does not match its element type");
      }
    } else if (const auto* store =
                   std::get_if<MirArrayStoreInstruction>(&instruction.data)) {
      verify_value(store->array, value_types, instruction.range);
      verify_value(store->index, value_types, instruction.range);
      verify_value(store->value, value_types, instruction.range);
      verify_value_type(store->index, *semantics_.find_type("int32"),
                        value_types, instruction.range);
      require_no_result(instruction);
      const std::optional<TypeId> element =
          array_element_type(store->array, value_types, instruction.range);
      if (element) {
        verify_value_type(store->value, *element, value_types,
                          instruction.range);
      }
    } else if (const auto* length =
                   std::get_if<MirArrayLengthInstruction>(&instruction.data)) {
      verify_value(length->array, value_types, instruction.range);
      static_cast<void>(
          array_element_type(length->array, value_types, instruction.range));
      require_result(instruction);
      if (instruction.type != *semantics_.find_type("int32")) {
        report(instruction.range, "array length does not have type int32");
      }
    } else if (const auto* meta =
                   std::get_if<MirStringMetaInstruction>(&instruction.data)) {
      verify_value(meta->string, value_types, instruction.range);
      verify_value_type(meta->string, semantics_.string_type(), value_types,
                        instruction.range);
      require_result(instruction);
      const TypeId expected = meta->query == StringMetaQuery::kIsEmpty
                                  ? semantics_.bool_type()
                                  : *semantics_.find_type("int32");
      if (instruction.type != expected) {
        report(instruction.range,
               "string meta query result has the wrong type");
      }
    } else if (const auto* meta =
                   std::get_if<MirObjectMetaInstruction>(&instruction.data)) {
      verify_value(meta->object, value_types, instruction.range);
      require_result(instruction);
      if (instruction.type != semantics_.string_type()) {
        report(instruction.range,
               "object meta query result does not have type string");
      }
    } else if (const auto* write =
                   std::get_if<MirIntegerWriteInstruction>(&instruction.data)) {
      verify_value(write->value, value_types, instruction.range);
      verify_value(write->destination, value_types, instruction.range);
      verify_value(write->offset, value_types, instruction.range);
      require_no_result(instruction);
      verify_value_type(write->offset, *semantics_.find_type("int32"),
                        value_types, instruction.range);
      const std::optional<TypeId> value_type =
          known_value_type(write->value, value_types);
      if (value_type && value_type->value < semantics_.types().size() &&
          !is_integer_type(semantics_.type(*value_type).kind)) {
        report(instruction.range,
               "integer endian write consumes a non-integer value");
      }
      const std::optional<TypeId> element = array_element_type(
          write->destination, value_types, instruction.range);
      if (element && *element != *semantics_.find_type("byte")) {
        report(instruction.range,
               "integer endian write destination is not byte[]");
      }
    } else if (const auto* read =
                   std::get_if<MirIntegerReadInstruction>(&instruction.data)) {
      verify_value(read->source, value_types, instruction.range);
      verify_value(read->offset, value_types, instruction.range);
      require_result(instruction);
      verify_value_type(read->offset, *semantics_.find_type("int32"),
                        value_types, instruction.range);
      const std::optional<TypeId> element =
          array_element_type(read->source, value_types, instruction.range);
      if (element && *element != *semantics_.find_type("byte")) {
        report(instruction.range, "integer endian read source is not byte[]");
      }
      if (instruction.type.value < semantics_.types().size() &&
          !is_integer_type(semantics_.type(instruction.type).kind)) {
        report(instruction.range,
               "integer endian read does not produce an integer");
      }
    } else if (const auto* unary =
                   std::get_if<MirUnaryInstruction>(&instruction.data)) {
      const auto operand_type = known_value_type(unary->operand, value_types);
      if (is_enum_type(operand_type) || is_enum_type(instruction.type) ||
          is_struct_type(operand_type) || is_struct_type(instruction.type)) {
        report(instruction.range, "enum value used by a unary operation");
      }
      verify_value(unary->operand, value_types, instruction.range);
      require_result(instruction);
      if (unary->operation == TokenKind::kTilde) {
        const std::optional<TypeId> operand_type =
            known_value_type(unary->operand, value_types);
        if (operand_type && operand_type->value < semantics_.types().size() &&
            (!is_integer_type(semantics_.type(*operand_type).kind) ||
             instruction.type != *operand_type)) {
          report(instruction.range,
                 "integer complement has incompatible types");
        }
      }
    } else if (const auto* binary =
                   std::get_if<MirBinaryInstruction>(&instruction.data)) {
      verify_value(binary->left, value_types, instruction.range);
      verify_value(binary->right, value_types, instruction.range);
      require_result(instruction);
      const std::optional<TypeId> left_type =
          known_value_type(binary->left, value_types);
      const std::optional<TypeId> right_type =
          known_value_type(binary->right, value_types);
      if ((is_enum_type(left_type) || is_enum_type(right_type) ||
           is_enum_type(instruction.type) || is_struct_type(left_type) ||
           is_struct_type(right_type) || is_struct_type(instruction.type)) &&
          (left_type != right_type ||
           instruction.type != semantics_.bool_type() ||
           (binary->operation != TokenKind::kEqualEqual &&
            binary->operation != TokenKind::kBangEqual))) {
        report(instruction.range,
               "enum binary operation must compare the same nominal type");
      }
      const bool is_bitwise = binary->operation == TokenKind::kAmpersand ||
                              binary->operation == TokenKind::kPipe ||
                              binary->operation == TokenKind::kCaret;
      const bool is_shift = binary->operation == TokenKind::kShiftLeft ||
                            binary->operation == TokenKind::kShiftRight;
      if (is_bitwise && left_type && right_type &&
          left_type->value < semantics_.types().size() &&
          right_type->value < semantics_.types().size() &&
          (!is_integer_type(semantics_.type(*left_type).kind) ||
           *left_type != *right_type || instruction.type != *left_type)) {
        report(instruction.range,
               "bitwise instruction has incompatible integer types");
      }
      if (is_shift && left_type && right_type &&
          left_type->value < semantics_.types().size() &&
          right_type->value < semantics_.types().size() &&
          (!is_integer_type(semantics_.type(*left_type).kind) ||
           !is_integer_type(semantics_.type(*right_type).kind) ||
           instruction.type != *left_type)) {
        report(instruction.range,
               "shift instruction has incompatible integer types");
      }
    } else if (const auto* conversion =
                   std::get_if<MirConvertInstruction>(&instruction.data)) {
      verify_value(conversion->value, value_types, instruction.range);
      require_result(instruction);
      const std::optional<TypeId> source_type =
          known_value_type(conversion->value, value_types);
      if (instruction.type != semantics_.error_type() &&
          instruction.type.value < semantics_.types().size()) {
        if (conversion->kind == MirConversionKind::kWidenNumeric) {
          if (source_type && *source_type != semantics_.error_type() &&
              source_type->value < semantics_.types().size() &&
              !can_widen_numeric(semantics_.type(*source_type).kind,
                                 semantics_.type(instruction.type).kind)) {
            report(instruction.range,
                   "numeric widening consumes incompatible types");
          }
        } else if (conversion->kind == MirConversionKind::kCheckedNumeric) {
          if (source_type && *source_type != semantics_.error_type() &&
              source_type->value < semantics_.types().size() &&
              (!is_numeric_type(semantics_.type(*source_type).kind) ||
               !is_numeric_type(semantics_.type(instruction.type).kind))) {
            report(instruction.range,
                   "checked numeric conversion consumes incompatible types");
          }
        } else if (conversion->kind == MirConversionKind::kWidenReference) {
          if (source_type &&
              !can_widen_reference(*source_type, instruction.type)) {
            report(instruction.range,
                   "reference widening consumes incompatible types");
          }
        } else if (conversion->kind == MirConversionKind::kToNullable) {
          const SemanticType& target = semantics_.type(instruction.type);
          if (target.kind != TypeKind::kNullable || !target.element_type) {
            report(instruction.range,
                   "nullable widening does not produce a nullable type");
          } else if (source_type && *source_type != semantics_.error_type() &&
                     *source_type != semantics_.null_type() &&
                     *source_type != *target.element_type) {
            report(instruction.range,
                   "nullable widening consumes an incompatible value");
          }
        } else if (source_type && *source_type != semantics_.error_type() &&
                   source_type->value < semantics_.types().size()) {
          const SemanticType& source = semantics_.type(*source_type);
          if (source.kind != TypeKind::kNullable || !source.element_type ||
              *source.element_type != instruction.type) {
            report(instruction.range,
                   "nullable narrowing consumes an incompatible value");
          }
        }
      }
    } else if (const auto* test =
                   std::get_if<MirTypeTestInstruction>(&instruction.data)) {
      verify_value(test->value, value_types, instruction.range);
      verify_type(test->target, instruction.range);
      if (is_enum_type(known_value_type(test->value, value_types)) ||
          is_enum_type(test->target) ||
          is_struct_type(known_value_type(test->value, value_types)) ||
          is_struct_type(test->target)) {
        report(instruction.range, "enum value used by a reference type test");
      }
      require_result(instruction);
      if (instruction.type != semantics_.bool_type()) {
        report(instruction.range, "type test does not have type bool");
      }
    } else if (const auto* cast =
                   std::get_if<MirCheckedCastInstruction>(&instruction.data)) {
      verify_value(cast->value, value_types, instruction.range);
      verify_type(cast->target, instruction.range);
      if (is_enum_type(known_value_type(cast->value, value_types)) ||
          is_enum_type(cast->target) ||
          is_struct_type(known_value_type(cast->value, value_types)) ||
          is_struct_type(cast->target)) {
        report(instruction.range, "enum value used by a reference cast");
      }
      require_result(instruction);
      if (instruction.type.value < semantics_.types().size()) {
        const SemanticType& result = semantics_.type(instruction.type);
        if (result.kind != TypeKind::kNullable ||
            result.element_type != cast->target) {
          report(instruction.range,
                 "checked cast result does not match its target");
        }
      }
    } else if (const auto* test =
                   std::get_if<MirIsNonNullInstruction>(&instruction.data)) {
      verify_value(test->value, value_types, instruction.range);
      require_result(instruction);
      if (instruction.type != semantics_.bool_type()) {
        report(instruction.range, "non-null test does not have type bool");
      }
      const std::optional<TypeId> source_type =
          known_value_type(test->value, value_types);
      if (source_type && *source_type != semantics_.error_type() &&
          source_type->value < semantics_.types().size()) {
        const SemanticType& source = semantics_.type(*source_type);
        if (source.kind != TypeKind::kNullable || !source.element_type) {
          report(instruction.range,
                 "non-null test consumes a non-nullable value");
        }
      }
    } else if (const auto* assertion =
                   std::get_if<MirNullAssertInstruction>(&instruction.data)) {
      verify_value(assertion->value, value_types, instruction.range);
      require_result(instruction);
      const std::optional<TypeId> source_type =
          known_value_type(assertion->value, value_types);
      if (source_type && *source_type != semantics_.error_type() &&
          source_type->value < semantics_.types().size()) {
        const SemanticType& source = semantics_.type(*source_type);
        if (source.kind != TypeKind::kNullable || !source.element_type ||
            *source.element_type != instruction.type) {
          report(instruction.range,
                 "non-null assertion consumes an incompatible value");
        }
      }
    } else if (const auto* call =
                   std::get_if<MirCallInstruction>(&instruction.data)) {
      verify_symbol(call->callable, instruction.range);
      verify_optional_value(call->receiver, value_types, instruction.range);
      if (call->kind == MirCallKind::kInstance && !call->receiver) {
        report(instruction.range, "instance call has no receiver");
      }
      if (call->kind != MirCallKind::kInstance && call->receiver &&
          !(call->kind == MirCallKind::kUnqualified &&
            call->struct_receiver == StructReceiverMode::kReadOnlyValue)) {
        report(instruction.range, "non-instance call has a receiver");
      }
      for (const MirValueId argument : call->arguments) {
        verify_value(argument, value_types, instruction.range);
      }
      if (call->receiver) {
        verify_member_receiver(*call->receiver, call->callable, value_types,
                               instruction.range);
      }
      if (call->callable.value < semantics_.symbols().size()) {
        const SemanticSymbol& callable = semantics_.symbol(call->callable);
        if (call->struct_receiver != receiver_mode(callable) ||
            (call->struct_receiver == StructReceiverMode::kReadOnlyValue &&
             (!call->receiver || call->dispatch != MirDispatchKind::kDirect)) ||
            (call->struct_receiver == StructReceiverMode::kConstruction &&
             (call->receiver || call->kind != MirCallKind::kConstructor))) {
          report(instruction.range,
                 "call has an invalid struct receiver contract");
        }
        const bool is_constructor = callable.kind == SymbolKind::kConstructor;
        const bool is_constructor_call =
            call->kind == MirCallKind::kConstructor ||
            call->kind == MirCallKind::kBaseConstructor;
        if (is_constructor_call != is_constructor) {
          report(instruction.range,
                 "call kind does not match its callable symbol");
        }
        if (!is_constructor && callable.kind != SymbolKind::kFunction) {
          report(instruction.range, "callable symbol is not callable");
        }
        if (callable.kind == SymbolKind::kFunction) {
          if (callable.is_static &&
              (call->kind == MirCallKind::kInstance ||
               call->kind == MirCallKind::kBaseQualified)) {
            report(instruction.range,
                   "static function call has an instance receiver");
          }
          if (!callable.is_static &&
              call->kind == MirCallKind::kClassQualified) {
            report(instruction.range,
                   "instance function call is class-qualified");
          }
          if (call->kind == MirCallKind::kBaseQualified &&
              (!call->receiver_is_self ||
               !is_base_qualified_callable(call->callable))) {
            report(instruction.range,
                   "base-qualified call does not target a direct-base "
                   "member");
          }
          const bool expects_virtual = callable.virtual_slot.has_value();
          const bool is_interface_dispatch =
              call->dispatch == MirDispatchKind::kInterface;
          if (is_interface_dispatch) {
            if (call->kind != MirCallKind::kInstance || !call->receiver ||
                !call->interface_file || !call->interface_slot ||
                call->interface_file->value >= semantics_.files().size()) {
              report(instruction.range,
                     "interface call has incomplete dispatch metadata");
            } else {
              const FileSemantics& interface_file =
                  semantics_.file(*call->interface_file);
              if (interface_file.kind != FileTypeKind::kInterface ||
                  *call->interface_slot >=
                      interface_file.interface_functions.size() ||
                  interface_file.interface_functions[*call->interface_slot] !=
                      call->callable) {
                report(instruction.range,
                       "interface call has invalid dispatch metadata");
              }
            }
          } else if (call->interface_file || call->interface_slot) {
            report(instruction.range,
                   "non-interface call retains interface dispatch metadata");
          }
          const bool should_dispatch_virtually =
              expects_virtual && call->kind != MirCallKind::kBaseQualified &&
              !(suppress_self_virtual_dispatch && call->receiver_is_self);
          if (call->dispatch == MirDispatchKind::kVirtual &&
              (!should_dispatch_virtually || callable.is_static)) {
            report(instruction.range,
                   "call has invalid virtual dispatch metadata");
          }
          if (should_dispatch_virtually && !is_interface_dispatch &&
              call->dispatch != MirDispatchKind::kVirtual) {
            report(instruction.range,
                   "virtual function was lowered as a direct call");
          }
          if (call->receiver_is_self && callable.is_static) {
            report(instruction.range,
                   "static function call has a self receiver");
          }
        }
        const TypeId expected_type = call->kind == MirCallKind::kBaseConstructor
                                         ? semantics_.void_type()
                                         : callable.type;
        if (instruction.type != expected_type) {
          report(instruction.range,
                 "call result type does not match its callable");
        }
        if (call->arguments.size() != callable.parameter_types.size()) {
          report(instruction.range,
                 "call argument count does not match its callable");
        }
        const std::size_t count =
            call->arguments.size() < callable.parameter_types.size()
                ? call->arguments.size()
                : callable.parameter_types.size();
        for (std::size_t index = 0; index < count; ++index) {
          verify_value_type(call->arguments[index],
                            callable.parameter_types[index], value_types,
                            instruction.range);
        }
      }
      if (instruction.type == semantics_.void_type()) {
        require_no_result(instruction);
      } else {
        require_result(instruction);
      }
    } else if (std::holds_alternative<MirInitializeFieldsInstruction>(
                   instruction.data)) {
      if (!initialization_allowed_) {
        report(instruction.range,
               "field initialization occurs outside a constructor");
      }
      require_no_result(instruction);
      if (instruction.type != semantics_.void_type()) {
        report(instruction.range,
               "field initialization instruction does not have void type");
      }
    } else if (const auto* phi =
                   std::get_if<MirPhiInstruction>(&instruction.data)) {
      require_result(instruction);
      if (phi->incoming.empty()) {
        report(instruction.range, "phi instruction has no incoming values");
      }
      for (const MirPhiIncoming& incoming : phi->incoming) {
        verify_block(incoming.predecessor, body, instruction.range);
        verify_value(incoming.value, value_types, instruction.range);
        verify_value_type(incoming.value, instruction.type, value_types,
                          instruction.range);
      }
    } else if (const auto* literal =
                   std::get_if<MirLiteralInstruction>(&instruction.data)) {
      require_result(instruction);
      if (is_struct_type(instruction.type)) {
        report(instruction.range,
               "struct cannot be represented by a scalar literal");
      }
      if ((literal->kind == LiteralKind::kEnum ||
           (instruction.type.value < semantics_.types().size() &&
            semantics_.type(instruction.type).kind == TypeKind::kEnum)) &&
          (literal->kind != LiteralKind::kEnum ||
           !enum_constant_tag(literal->lexeme, instruction.type, semantics_))) {
        report(instruction.range, "invalid nominal enum constant");
      }
    }
  }

  void verify_switch(const MirSwitchTerminator& selection, const MirBody& body,
                     const std::vector<std::optional<TypeId>>& value_types,
                     SourceRange range) {
    verify_value(selection.selector, value_types, range);
    verify_type(selection.selector_type, range);
    verify_value_type(selection.selector, selection.selector_type, value_types,
                      range);
    verify_block(selection.default_block, body, range);
    if (selection.cases.size() > kMaxSwitchLabels) {
      report(range, "switch exceeds the 65536-label limit");
      return;
    }
    if (selection.selector_type.value >= semantics_.types().size()) return;
    const auto& type = semantics_.type(selection.selector_type);
    const bool is_enum = type.kind == TypeKind::kEnum;
    if (!is_enum && !is_integer_type(type.kind)) {
      report(range, "switch selector must be an integer or enum");
      return;
    }
    std::size_t enum_size = 0;
    if (is_enum) {
      if (!type.file || type.file->value >= semantics_.files().size() ||
          semantics_.file(*type.file).type != selection.selector_type ||
          semantics_.file(*type.file).kind != FileTypeKind::kEnum) {
        report(range, "switch enum has an invalid nominal owner");
        return;
      }
      enum_size = semantics_.file(*type.file).enum_cases.size();
      if (!selection.invalid_block) {
        report(range, "enum switch has no invalid-tag trap");
      } else {
        verify_block(*selection.invalid_block, body, range);
        if (selection.invalid_block->value < body.blocks.size()) {
          const auto& invalid = body.blocks[selection.invalid_block->value];
          if (!invalid.instructions.empty() ||
              !std::holds_alternative<MirTrapTerminator>(
                  invalid.terminator.data))
            report(range,
                   "enum switch invalid-tag target is not an empty trap block");
        }
        if (selection.default_block == *selection.invalid_block &&
            selection.cases.size() != enum_size)
          report(range, "enum switch without default is not exhaustive");
      }
    } else if (selection.invalid_block) {
      report(range, "integer switch cannot have an enum invalid-tag target");
    }
    std::optional<std::uint64_t> previous;
    for (const auto& entry : selection.cases) {
      verify_block(entry.target, body, range);
      if (entry.value.type != selection.selector_type)
        report(range, "switch case type differs from selector type");
      if (is_enum ? entry.value.bits >= enum_size
                  : !is_valid_integer_bits(entry.value.bits, type.kind))
        report(range, "switch case has an invalid normalized value");
      if (previous && *previous >= entry.value.bits)
        report(range,
               "switch cases must be distinct and sorted by normalized bits");
      previous = entry.value.bits;
    }
  }

  void verify_terminator(const MirTerminator& terminator, const MirBody& body,
                         const std::vector<std::optional<TypeId>>& value_types,
                         TypeId return_type) {
    if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
      verify_block(jump->target, body, terminator.range);
    } else if (const auto* branch =
                   std::get_if<MirBranchTerminator>(&terminator.data)) {
      verify_value(branch->condition, value_types, terminator.range);
      verify_value_type(branch->condition, semantics_.bool_type(), value_types,
                        terminator.range);
      verify_block(branch->then_block, body, terminator.range);
      verify_block(branch->else_block, body, terminator.range);
    } else if (const auto* selection =
                   std::get_if<MirSwitchTerminator>(&terminator.data)) {
      verify_switch(*selection, body, value_types, terminator.range);
    } else if (const auto* return_terminator =
                   std::get_if<MirReturnTerminator>(&terminator.data)) {
      verify_optional_value(return_terminator->value, value_types,
                            terminator.range);
      if (return_type == semantics_.void_type() && return_terminator->value) {
        report(terminator.range, "void body returns a value");
      }
      if (return_type != semantics_.void_type() &&
          return_type != semantics_.error_type() && !return_terminator->value) {
        report(terminator.range, "value body returns without a value");
      }
      if (return_terminator->value) {
        verify_value_type(*return_terminator->value, return_type, value_types,
                          terminator.range);
      }
    }
  }

  void require_result(const MirInstruction& instruction) {
    if (!instruction.result) {
      report(instruction.range, "value instruction has no result");
    }
    if (instruction.type == semantics_.void_type()) {
      report(instruction.range, "value instruction has the void type");
    }
  }

  void require_no_result(const MirInstruction& instruction) {
    if (instruction.result) {
      report(instruction.range, "effect instruction unexpectedly has a result");
    }
    if (instruction.type != semantics_.void_type()) {
      report(instruction.range, "effect instruction has a value type");
    }
  }

  void verify_type(TypeId type, SourceRange range) {
    if (type.value >= semantics_.types().size()) {
      report(range, "instruction references an unknown type");
    }
  }

  void verify_symbol(SymbolId symbol, SourceRange range) {
    if (symbol.value >= semantics_.symbols().size()) {
      report(range, "instruction references an unknown symbol");
    }
  }

  TypeId symbol_type(SymbolId symbol, TypeId fallback) const {
    return symbol.value < semantics_.symbols().size()
               ? semantics_.symbol(symbol).type
               : fallback;
  }

  std::optional<TypeId> array_element_type(
      MirValueId array, const std::vector<std::optional<TypeId>>& value_types,
      SourceRange range) {
    const std::optional<TypeId> array_type =
        known_value_type(array, value_types);
    if (!array_type || array_type->value >= semantics_.types().size()) {
      return std::nullopt;
    }
    const SemanticType& type = semantics_.type(*array_type);
    if (type.kind != TypeKind::kArray || !type.element_type) {
      report(range, "array instruction consumes a non-array value");
      return std::nullopt;
    }
    return type.element_type;
  }

  void verify_symbol_type(SymbolId symbol, TypeId type, SourceRange range) {
    if (symbol.value < semantics_.symbols().size() &&
        semantics_.symbol(symbol).type != type &&
        type != semantics_.error_type()) {
      report(range, "instruction type does not match its symbol");
    }
  }

  std::optional<TypeId> known_value_type(
      MirValueId value,
      const std::vector<std::optional<TypeId>>& value_types) const {
    if (value.value >= value_types.size()) {
      return std::nullopt;
    }
    return value_types[value.value];
  }

  void verify_value_type(MirValueId value, TypeId expected,
                         const std::vector<std::optional<TypeId>>& value_types,
                         SourceRange range) {
    const std::optional<TypeId> actual = known_value_type(value, value_types);
    if (actual && *actual != expected && *actual != semantics_.error_type() &&
        expected != semantics_.error_type()) {
      report(range, "value type does not match its use");
    }
  }

  bool is_non_null_reference(TypeId type) const {
    if (type.value >= semantics_.types().size()) {
      return false;
    }
    const TypeKind kind = semantics_.type(type).kind;
    return kind == TypeKind::kString || kind == TypeKind::kObject ||
           kind == TypeKind::kFileClass || kind == TypeKind::kInterface ||
           kind == TypeKind::kArray;
  }

  bool can_widen_reference(TypeId source, TypeId target) const {
    if (source == semantics_.error_type() ||
        target == semantics_.error_type()) {
      return true;
    }
    if (source == target) {
      return true;
    }
    if (target == semantics_.object_type()) {
      return is_non_null_reference(source);
    }
    if (source.value >= semantics_.types().size() ||
        target.value >= semantics_.types().size()) {
      return false;
    }
    const SemanticType& source_type = semantics_.type(source);
    const SemanticType& target_type = semantics_.type(target);
    if (source_type.kind == TypeKind::kNullable && source_type.element_type &&
        target_type.kind == TypeKind::kNullable && target_type.element_type) {
      return can_widen_reference(*source_type.element_type,
                                 *target_type.element_type);
    }
    if ((source_type.kind == TypeKind::kFileClass ||
         source_type.kind == TypeKind::kInterface) &&
        source_type.file && target_type.kind == TypeKind::kInterface &&
        target_type.file) {
      const std::vector<FileId>& interfaces =
          semantics_.file(*source_type.file).interfaces;
      return std::ranges::find(interfaces, *target_type.file) !=
             interfaces.end();
    }
    if (source_type.kind != TypeKind::kFileClass || !source_type.file ||
        target_type.kind != TypeKind::kFileClass || !target_type.file) {
      return false;
    }
    std::optional<FileId> current =
        semantics_.file(*source_type.file).base_file;
    for (std::size_t depth = 0; current && depth < semantics_.files().size();
         ++depth) {
      if (*current == *target_type.file) {
        return true;
      }
      current = semantics_.file(*current).base_file;
    }
    return false;
  }

  std::optional<TypeId> verify_storage_path(
      const MirStoragePath& path,
      const std::vector<std::optional<TypeId>>& value_types, SourceRange range,
      bool write, bool construction) {
    if (path.symbol.has_value() == path.object.has_value() ||
        (path.index && !path.object)) {
      report(range, "storage path must have exactly one valid root");
      return std::nullopt;
    }
    std::optional<TypeId> current;
    bool read_only = false;
    bool self = false;
    if (path.symbol) {
      verify_symbol(*path.symbol, range);
      if (path.symbol->value >= semantics_.symbols().size())
        return std::nullopt;
      const auto& symbol = semantics_.symbol(*path.symbol);
      self = symbol.kind == SymbolKind::kSelf;
      if ((symbol.kind != SymbolKind::kLocal &&
           symbol.kind != SymbolKind::kParameter && !self) ||
          symbol.file != current_file_ || !is_struct_type(symbol.type)) {
        report(range,
               "storage path symbol is not an inline value in this file");
        return std::nullopt;
      }
      if ((symbol.kind == SymbolKind::kLocal &&
           !current_locals_.contains(path.symbol->value)) ||
          (symbol.kind == SymbolKind::kParameter &&
           !current_parameters_.contains(path.symbol->value))) {
        report(range, "storage path refers to another callable's storage");
      }
      current = symbol.type;
      read_only = symbol.is_final || (self && !construction);
      if (self && path.fields.empty() && write)
        report(range, "storage path replaces self");
    } else {
      verify_value(*path.object, value_types, range);
      current = known_value_type(*path.object, value_types);
      if (path.index) {
        verify_value(*path.index, value_types, range);
        verify_value_type(*path.index, *semantics_.find_type("int32"),
                          value_types, range);
        current = array_element_type(*path.object, value_types, range);
      } else if (current && current->value < semantics_.types().size() &&
                 semantics_.type(*current).kind != TypeKind::kFileClass) {
        report(range, "storage path object is not a managed class reference");
        return std::nullopt;
      }
    }
    for (std::size_t index = 0; index < path.fields.size(); ++index) {
      if (!current || current->value >= semantics_.types().size())
        return std::nullopt;
      const auto& owner_type = semantics_.type(*current);
      const SymbolId field_id = path.fields[index];
      verify_symbol(field_id, range);
      if (field_id.value >= semantics_.symbols().size()) return std::nullopt;
      const auto& field = semantics_.symbol(field_id);
      const bool class_root = path.object && !path.index && index == 0;
      if (!owner_type.file || !field.file || field.kind != SymbolKind::kField ||
          field.is_static ||
          (class_root ? !is_file_subtype(*owner_type.file, *field.file)
                      : owner_type.kind != TypeKind::kStruct ||
                            owner_type.file != field.file)) {
        report(
            range,
            "storage projection has an unrelated field or crosses a reference");
        return std::nullopt;
      }
      const bool initializes_field =
          self && construction && index == 0 && path.fields.size() == 1;
      read_only = read_only || (field.is_final && !initializes_field);
      current = field.type;
    }
    if (write && read_only)
      report(range, "storage write targets a read-only value");
    return current;
  }

  void verify_member_receiver(
      MirValueId receiver, SymbolId member,
      const std::vector<std::optional<TypeId>>& value_types,
      SourceRange range) {
    if (receiver.value >= value_types.size() || !value_types[receiver.value] ||
        member.value >= semantics_.symbols().size()) {
      return;
    }
    const TypeId receiver_type = *value_types[receiver.value];
    if (receiver_type.value >= semantics_.types().size()) {
      return;
    }
    const SemanticType& type = semantics_.type(receiver_type);
    const std::optional<FileId> owner = semantics_.symbol(member).file;
    bool related = false;
    if (type.kind == TypeKind::kStruct && type.file && owner) {
      related = type.file == owner;
    } else if (type.kind == TypeKind::kFileClass && type.file && owner) {
      related = is_file_subtype(*type.file, *owner);
    } else if (type.kind == TypeKind::kInterface && type.file && owner) {
      const std::vector<FileId>& interfaces =
          semantics_.file(*type.file).interfaces;
      related = std::ranges::find(interfaces, *owner) != interfaces.end();
    }
    if (!related) {
      report(range, "member receiver is unrelated to its declaring class");
    }
  }

  bool is_file_subtype(FileId subtype, FileId supertype) const {
    std::optional<FileId> current = subtype;
    for (std::size_t depth = 0; current && depth < semantics_.files().size();
         ++depth) {
      if (*current == supertype) {
        return true;
      }
      current = semantics_.file(*current).base_file;
    }
    return false;
  }

  bool is_base_qualified_callable(SymbolId callable_id) const {
    if (!current_file_ || current_file_->value >= semantics_.files().size() ||
        callable_id.value >= semantics_.symbols().size()) {
      return false;
    }
    const SemanticSymbol& callable = semantics_.symbol(callable_id);
    if (callable.kind != SymbolKind::kFunction || callable.is_static ||
        callable.visibility != Visibility::kPublic) {
      return false;
    }

    std::optional<FileId> owner = semantics_.file(*current_file_).base_file;
    for (std::size_t depth = 0;
         owner && owner->value < semantics_.files().size() &&
         depth < semantics_.files().size();
         ++depth) {
      const FileSemantics& file = semantics_.file(*owner);
      bool has_named_member = false;
      for (const SymbolId field_id : file.fields) {
        has_named_member = has_named_member ||
                           semantics_.symbol(field_id).name == callable.name;
      }
      for (const SymbolId function_id : file.functions) {
        if (semantics_.symbol(function_id).name != callable.name) {
          continue;
        }
        has_named_member = true;
        if (function_id == callable_id) {
          return true;
        }
      }
      if (has_named_member) {
        return false;
      }
      owner = file.base_file;
    }
    return false;
  }

  void verify_value(MirValueId value,
                    const std::vector<std::optional<TypeId>>& value_types,
                    SourceRange range) {
    if (value.value >= value_types.size() || !value_types[value.value]) {
      report(range, "instruction references an unknown value");
    }
  }

  void verify_optional_value(
      std::optional<MirValueId> value,
      const std::vector<std::optional<TypeId>>& value_types,
      SourceRange range) {
    if (value) {
      verify_value(*value, value_types, range);
    }
  }

  void verify_block(MirBlockId block, const MirBody& body, SourceRange range) {
    if (block.value >= body.blocks.size()) {
      report(range, "terminator references an unknown basic block");
    }
  }

  SourceRange symbol_range(SymbolId symbol) const {
    if (symbol.value < semantics_.symbols().size()) {
      return semantics_.symbol(symbol).range;
    }
    return fallback_range();
  }

  static SourceRange fallback_range() noexcept {
    return point_range(SourceLocation{"<mir>", 0, 1, 1});
  }

  void report(SourceRange range, std::string message) {
    diagnostics_.error(range, "internal MIR verification error: " + message);
    is_valid_ = false;
  }

  const MirModule& mir_;
  const SemanticModel& semantics_;
  DiagnosticEngine& diagnostics_;
  std::optional<FileId> current_file_;
  std::set<std::size_t> current_parameters_;
  std::set<std::size_t> current_locals_;
  bool initialization_allowed_{false};
  bool is_valid_{true};
};

}  // namespace

bool verify_mir(const MirModule& mir, const SemanticModel& semantics,
                DiagnosticEngine& diagnostics) {
  return MirVerifier{mir, semantics, diagnostics}.run();
}

}  // namespace cloth
