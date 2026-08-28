#include "cloth/mir/mir_verifier.h"

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <optional>
#include <string>
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
  void verify_file(const MirFileClass& file, std::size_t file_index) {
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
      if (file.fields.size() != semantic_file.fields.size() ||
          file.functions.size() != semantic_file.functions.size() ||
          file.constructors.size() != semantic_file.constructors.size()) {
        report(range, "file member counts do not match semantics");
      }
    }
    verify_symbol(file.symbol, range);
    for (const MirField& field : file.fields) {
      verify_symbol(field.symbol, range);
      if (field.initializer) {
        verify_body(*field.initializer,
                    symbol_type(field.symbol, semantics_.error_type()));
      }
    }
    for (const MirCallable& function : file.functions) {
      verify_callable(function, SymbolKind::kFunction);
    }
    for (const MirCallable& constructor : file.constructors) {
      verify_callable(constructor, SymbolKind::kConstructor);
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

  void verify_callable(const MirCallable& callable, SymbolKind expected_kind) {
    const SourceRange range = symbol_range(callable.symbol);
    verify_symbol(callable.symbol, range);
    TypeId return_type = semantics_.error_type();
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
    }
    for (const SymbolId parameter : callable.parameters) {
      verify_symbol(parameter, range);
      if (parameter.value < semantics_.symbols().size() &&
          semantics_.symbol(parameter).kind != SymbolKind::kParameter) {
        report(range, "callable parameter has the wrong symbol kind");
      }
    }
    verify_body(callable.body, return_type);
  }

  void verify_body(const MirBody& body, TypeId return_type) {
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

    for (std::size_t block_index = 0; block_index < body.blocks.size();
         ++block_index) {
      const MirBasicBlock& block = body.blocks[block_index];
      for (const MirInstruction& instruction : block.instructions) {
        verify_instruction(instruction, body, value_types);
      }
      verify_terminator(block.terminator, body, value_types, return_type);
    }
    verify_reachability(body);
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
      const MirTerminatorData& terminator =
          body.blocks[block_id.value].terminator.data;
      if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator)) {
        if (jump->target.value < body.blocks.size()) {
          worklist.push_back(jump->target);
        }
      } else if (const auto* branch =
                     std::get_if<MirBranchTerminator>(&terminator)) {
        if (branch->then_block.value < body.blocks.size()) {
          worklist.push_back(branch->then_block);
        }
        if (branch->else_block.value < body.blocks.size()) {
          worklist.push_back(branch->else_block);
        }
      }
    }
    for (std::size_t index = 0; index < body.blocks.size(); ++index) {
      if (body.blocks[index].is_reachable != reachable[index]) {
        report(body.range, "basic-block reachability flag is inconsistent");
      }
    }
  }

  void verify_instruction(
      const MirInstruction& instruction, const MirBody& body,
      const std::vector<std::optional<TypeId>>& value_types) {
    if (const auto* load =
            std::get_if<MirLoadSymbolInstruction>(&instruction.data)) {
      verify_symbol(load->symbol, instruction.range);
      require_result(instruction);
      verify_symbol_type(load->symbol, instruction.type, instruction.range);
      if (load->symbol.value < semantics_.symbols().size()) {
        const SemanticSymbol& symbol = semantics_.symbol(load->symbol);
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
      if (load->member.value < semantics_.symbols().size() &&
          semantics_.symbol(load->member).is_static) {
        report(instruction.range, "static field was lowered as a member load");
      }
    } else if (const auto* store =
                   std::get_if<MirStoreMemberInstruction>(&instruction.data)) {
      verify_value(store->object, value_types, instruction.range);
      verify_symbol(store->member, instruction.range);
      verify_value(store->value, value_types, instruction.range);
      require_no_result(instruction);
      verify_value_type(store->value,
                        symbol_type(store->member, semantics_.error_type()),
                        value_types, instruction.range);
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
    } else if (const auto* unary =
                   std::get_if<MirUnaryInstruction>(&instruction.data)) {
      verify_value(unary->operand, value_types, instruction.range);
      require_result(instruction);
    } else if (const auto* binary =
                   std::get_if<MirBinaryInstruction>(&instruction.data)) {
      verify_value(binary->left, value_types, instruction.range);
      verify_value(binary->right, value_types, instruction.range);
      require_result(instruction);
    } else if (const auto* conversion =
                   std::get_if<MirConvertInstruction>(&instruction.data)) {
      verify_value(conversion->value, value_types, instruction.range);
      require_result(instruction);
      const std::optional<TypeId> source_type =
          known_value_type(conversion->value, value_types);
      if (instruction.type != semantics_.error_type() &&
          instruction.type.value < semantics_.types().size()) {
        if (conversion->kind == MirConversionKind::kWidenReference) {
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
      require_result(instruction);
      if (instruction.type != semantics_.bool_type()) {
        report(instruction.range, "type test does not have type bool");
      }
    } else if (const auto* cast =
                   std::get_if<MirCheckedCastInstruction>(&instruction.data)) {
      verify_value(cast->value, value_types, instruction.range);
      verify_type(cast->target, instruction.range);
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
      if (call->kind != MirCallKind::kInstance && call->receiver) {
        report(instruction.range, "non-instance call has a receiver");
      }
      for (const MirValueId argument : call->arguments) {
        verify_value(argument, value_types, instruction.range);
      }
      if (call->callable.value < semantics_.symbols().size()) {
        const SemanticSymbol& callable = semantics_.symbol(call->callable);
        const bool is_constructor = callable.kind == SymbolKind::kConstructor;
        if ((call->kind == MirCallKind::kConstructor) != is_constructor) {
          report(instruction.range,
                 "call kind does not match its callable symbol");
        }
        if (!is_constructor && callable.kind != SymbolKind::kFunction) {
          report(instruction.range, "callable symbol is not callable");
        }
        if (callable.kind == SymbolKind::kFunction) {
          if (callable.is_static && call->kind == MirCallKind::kInstance) {
            report(instruction.range,
                   "static function call has an instance receiver");
          }
          if (!callable.is_static &&
              call->kind == MirCallKind::kClassQualified) {
            report(instruction.range,
                   "instance function call is class-qualified");
          }
        }
        if (instruction.type != callable.type) {
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
    } else if (std::holds_alternative<MirLiteralInstruction>(
                   instruction.data)) {
      require_result(instruction);
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
           kind == TypeKind::kFileClass || kind == TypeKind::kArray;
  }

  bool can_widen_reference(TypeId source, TypeId target) const {
    if (source == semantics_.error_type() ||
        target == semantics_.error_type()) {
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
    return source_type.kind == TypeKind::kNullable &&
           target_type.kind == TypeKind::kNullable &&
           source_type.element_type && target_type.element_type &&
           (*source_type.element_type == *target_type.element_type ||
            (*target_type.element_type == semantics_.object_type() &&
             is_non_null_reference(*source_type.element_type)));
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
  bool is_valid_{true};
};

}  // namespace

bool verify_mir(const MirModule& mir, const SemanticModel& semantics,
                DiagnosticEngine& diagnostics) {
  return MirVerifier{mir, semantics, diagnostics}.run();
}

}  // namespace cloth
