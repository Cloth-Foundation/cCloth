#include "cloth/backend/llvm_ir.h"

#include "cloth/abi/abi.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/lexer/literal.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir.h"
#include "cloth/runtime/runtime.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

std::string encode_name(std::string_view name) {
  return std::to_string(name.size()) + "_" + std::string{name};
}

std::string field_initializer_name(std::string_view class_name,
                                   std::string_view field_name) {
  return "_C1I" + encode_name(class_name) + encode_name(field_name);
}

std::uint32_t decode_character(std::string_view lexeme) noexcept {
  if (lexeme.size() < 3) {
    return 0;
  }
  const char value = lexeme[1] == '\\' && lexeme.size() >= 4
                         ? decode_escape_character(lexeme[2])
                         : lexeme[1];
  return static_cast<unsigned char>(value);
}

std::optional<std::string> lower_scalar_literal(
    const MirLiteralInstruction& literal, const AbiTypeLayout& type) {
  switch (literal.kind) {
    case LiteralKind::kInteger: {
      std::uint64_t parsed = 0;
      const char* const begin = literal.lexeme.data();
      const char* const end = begin + literal.lexeme.size();
      const auto result = std::from_chars(begin, end, parsed);
      const std::uint64_t maximum =
          type.bit_width == 0 ? 0
          : type.bit_width >= 64
              ? static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
              : (std::uint64_t{1} << (type.bit_width - 1)) - 1;
      if (type.bit_width == 0 || result.ec != std::errc{} ||
          result.ptr != end || parsed > maximum) {
        return std::nullopt;
      }
      return std::to_string(parsed);
    }
    case LiteralKind::kFloat: {
      double parsed = 0.0;
      const char* const begin = literal.lexeme.data();
      const char* const end = begin + literal.lexeme.size();
      const auto parsed_result =
          std::from_chars(begin, end, parsed, std::chars_format::general);
      std::array<char, 128> buffer{};
      const auto formatted =
          std::to_chars(buffer.data(), buffer.data() + buffer.size(), parsed,
                        std::chars_format::scientific, 17);
      if (parsed_result.ec != std::errc{} || parsed_result.ptr != end ||
          formatted.ec != std::errc{}) {
        return std::nullopt;
      }
      return std::string{buffer.data(), formatted.ptr};
    }
    case LiteralKind::kCharacter:
      return std::to_string(decode_character(literal.lexeme));
    case LiteralKind::kBoolean:
      return literal.lexeme;
    case LiteralKind::kString:
    case LiteralKind::kNull:
      return std::nullopt;
  }
  return std::nullopt;
}

const MirInstruction* returned_instruction(const MirBody& body) {
  for (const MirBasicBlock& block : body.blocks) {
    const auto* returned =
        std::get_if<MirReturnTerminator>(&block.terminator.data);
    if (returned == nullptr || !returned->value) {
      continue;
    }
    for (const MirBasicBlock& candidate_block : body.blocks) {
      for (const MirInstruction& instruction : candidate_block.instructions) {
        if (instruction.result == returned->value) {
          return &instruction;
        }
      }
    }
  }
  return nullptr;
}

std::vector<MirValueId> instruction_value_uses(
    const MirInstruction& instruction) {
  std::vector<MirValueId> uses;
  if (const auto* declaration =
          std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
    if (declaration->initializer) {
      uses.push_back(*declaration->initializer);
    }
  } else if (const auto* store =
                 std::get_if<MirStoreSymbolInstruction>(&instruction.data)) {
    uses.push_back(store->value);
  } else if (const auto* load =
                 std::get_if<MirLoadMemberInstruction>(&instruction.data)) {
    uses.push_back(load->object);
  } else if (const auto* store =
                 std::get_if<MirStoreMemberInstruction>(&instruction.data)) {
    uses.push_back(store->object);
    uses.push_back(store->value);
  } else if (const auto* array =
                 std::get_if<MirArrayLiteralInstruction>(&instruction.data)) {
    uses = array->elements;
  } else if (const auto* load =
                 std::get_if<MirArrayLoadInstruction>(&instruction.data)) {
    uses.push_back(load->array);
    uses.push_back(load->index);
  } else if (const auto* store =
                 std::get_if<MirArrayStoreInstruction>(&instruction.data)) {
    uses.push_back(store->array);
    uses.push_back(store->index);
    uses.push_back(store->value);
  } else if (const auto* length =
                 std::get_if<MirArrayLengthInstruction>(&instruction.data)) {
    uses.push_back(length->array);
  } else if (const auto* meta =
                 std::get_if<MirStringMetaInstruction>(&instruction.data)) {
    uses.push_back(meta->string);
  } else if (const auto* meta =
                 std::get_if<MirObjectMetaInstruction>(&instruction.data)) {
    uses.push_back(meta->object);
  } else if (const auto* unary =
                 std::get_if<MirUnaryInstruction>(&instruction.data)) {
    uses.push_back(unary->operand);
  } else if (const auto* binary =
                 std::get_if<MirBinaryInstruction>(&instruction.data)) {
    uses.push_back(binary->left);
    uses.push_back(binary->right);
  } else if (const auto* conversion =
                 std::get_if<MirConvertInstruction>(&instruction.data)) {
    uses.push_back(conversion->value);
  } else if (const auto* test =
                 std::get_if<MirIsNonNullInstruction>(&instruction.data)) {
    uses.push_back(test->value);
  } else if (const auto* assertion =
                 std::get_if<MirNullAssertInstruction>(&instruction.data)) {
    uses.push_back(assertion->value);
  } else if (const auto* test =
                 std::get_if<MirTypeTestInstruction>(&instruction.data)) {
    uses.push_back(test->value);
  } else if (const auto* cast =
                 std::get_if<MirCheckedCastInstruction>(&instruction.data)) {
    uses.push_back(cast->value);
  } else if (const auto* call =
                 std::get_if<MirCallInstruction>(&instruction.data)) {
    if (call->receiver) {
      uses.push_back(*call->receiver);
    }
    uses.insert(uses.end(), call->arguments.begin(), call->arguments.end());
  }
  return uses;
}

std::vector<MirValueId> terminator_value_uses(const MirTerminator& terminator) {
  if (const auto* branch = std::get_if<MirBranchTerminator>(&terminator.data)) {
    return {branch->condition};
  }
  if (const auto* returned = std::get_if<MirReturnTerminator>(&terminator.data);
      returned != nullptr && returned->value) {
    return {*returned->value};
  }
  return {};
}

std::vector<MirBlockId> terminator_successors(const MirTerminator& terminator) {
  if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
    return {jump->target};
  }
  if (const auto* branch = std::get_if<MirBranchTerminator>(&terminator.data)) {
    return {branch->then_block, branch->else_block};
  }
  return {};
}

struct GcLiveSet {
  std::vector<bool> values;
  std::vector<bool> symbols;

  friend bool operator==(const GcLiveSet&, const GcLiveSet&) = default;
};

std::string llvm_string_bytes(std::string_view value) {
  std::ostringstream output;
  output << std::uppercase << std::hex << std::setfill('0');
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character >= 0x20 && character <= 0x7e && character != '"' &&
        character != '\\') {
      output << static_cast<char>(character);
    } else {
      output << '\\' << std::setw(2) << static_cast<unsigned int>(character);
    }
  }
  return output.str();
}

class ModuleEmitter;

class BodyEmitter {
 public:
  BodyEmitter(ModuleEmitter& module, const MirBody& body,
              const AbiFileClass& file, TypeId return_type,
              std::string receiver, bool is_constructor,
              const AbiCallable* callable);

  [[nodiscard]] std::string emit();

 private:
  void prepare_values();
  void collect_storage();
  void collect_gc_roots();
  void analyze_gc_liveness();
  void emit_prologue(std::ostringstream& output);
  void emit_gc_value_root(const MirInstruction& instruction,
                          std::ostringstream& output) const;
  void emit_gc_block_entry_clears(std::size_t block,
                                  std::ostringstream& output) const;
  void emit_gc_dead_roots(std::size_t block, std::size_t instruction_index,
                          const MirInstruction& instruction,
                          std::ostringstream& output) const;
  void emit_gc_epilogue(std::ostringstream& output) const;
  void emit_field_initializers(std::ostringstream& output);
  void emit_instruction(const MirInstruction& instruction,
                        std::ostringstream& output);
  void emit_literal(const MirInstruction& instruction,
                    const MirLiteralInstruction& literal,
                    std::ostringstream& output);
  void emit_load_symbol(const MirInstruction& instruction,
                        const MirLoadSymbolInstruction& load,
                        std::ostringstream& output);
  void emit_member_load(const MirInstruction& instruction,
                        const MirLoadMemberInstruction& load,
                        std::ostringstream& output);
  void emit_member_store(const MirInstruction& instruction,
                         const MirStoreMemberInstruction& store,
                         std::ostringstream& output);
  void emit_array_literal(const MirInstruction& instruction,
                          const MirArrayLiteralInstruction& array,
                          std::ostringstream& output);
  void emit_array_load(const MirInstruction& instruction,
                       const MirArrayLoadInstruction& load,
                       std::ostringstream& output);
  void emit_array_store(const MirInstruction& instruction,
                        const MirArrayStoreInstruction& store,
                        std::ostringstream& output);
  void emit_array_length(const MirInstruction& instruction,
                         const MirArrayLengthInstruction& length,
                         std::ostringstream& output);
  void emit_string_meta(const MirInstruction& instruction,
                        const MirStringMetaInstruction& meta,
                        std::ostringstream& output);
  void emit_object_meta(const MirInstruction& instruction,
                        const MirObjectMetaInstruction& meta,
                        std::ostringstream& output);
  void emit_unary(const MirInstruction& instruction,
                  const MirUnaryInstruction& unary, std::ostringstream& output);
  void emit_binary(const MirInstruction& instruction,
                   const MirBinaryInstruction& binary,
                   std::ostringstream& output);
  void emit_is_non_null(const MirInstruction& instruction,
                        const MirIsNonNullInstruction& test,
                        std::ostringstream& output);
  void emit_null_assert(const MirInstruction& instruction,
                        const MirNullAssertInstruction& assertion,
                        std::ostringstream& output);
  void emit_type_test(const MirInstruction& instruction,
                      const MirTypeTestInstruction& test,
                      std::ostringstream& output);
  void emit_checked_cast(const MirInstruction& instruction,
                         const MirCheckedCastInstruction& cast,
                         std::ostringstream& output);
  void emit_type_condition(std::string_view result, MirValueId value,
                           TypeId target, SourceRange range,
                           std::ostringstream& output);
  void emit_call(const MirInstruction& instruction,
                 const MirCallInstruction& call, std::ostringstream& output);
  void emit_phi(const MirInstruction& instruction, const MirPhiInstruction& phi,
                std::ostringstream& output);
  void emit_terminator(const MirTerminator& terminator, bool is_reachable,
                       std::ostringstream& output);

  [[nodiscard]] std::string value(MirValueId id) const;
  [[nodiscard]] TypeId value_type(MirValueId id) const;
  [[nodiscard]] std::string result_name(
      const MirInstruction& instruction) const;
  [[nodiscard]] std::string symbol_address(SymbolId symbol) const;
  [[nodiscard]] std::string argument_name(SymbolId symbol) const;
  [[nodiscard]] std::string gc_value_address(MirValueId value) const;
  [[nodiscard]] bool has_gc_symbol_root(SymbolId symbol) const noexcept;
  [[nodiscard]] bool is_string_like(TypeId type) const noexcept;
  [[nodiscard]] bool has_receiver_root() const noexcept;
  [[nodiscard]] std::size_t gc_root_count() const noexcept;
  [[nodiscard]] std::string next_address();

  ModuleEmitter& module_;
  const MirBody& body_;
  const AbiFileClass& file_;
  TypeId return_type_;
  std::string receiver_;
  bool is_constructor_;
  const AbiCallable* callable_;
  std::vector<std::string> values_;
  std::vector<TypeId> value_types_;
  std::vector<SymbolId> storage_symbols_;
  std::vector<SymbolId> gc_symbol_roots_;
  std::vector<bool> gc_value_roots_;
  std::vector<GcLiveSet> gc_live_in_;
  std::vector<GcLiveSet> gc_live_out_;
  std::vector<GcLiveSet> gc_live_after_phis_;
  std::vector<std::vector<GcLiveSet>> gc_live_after_instructions_;
  std::size_t gc_value_root_count_{0};
  std::size_t address_count_{0};
};

class ModuleEmitter {
 public:
  ModuleEmitter(const MirModule& mir, const AbiModule& abi,
                const SemanticModel& semantics, DiagnosticEngine& diagnostics,
                LlvmIrOptions options)
      : mir_(mir),
        abi_(abi),
        semantics_(semantics),
        diagnostics_(diagnostics),
        options_(options) {}

  std::optional<LlvmIrModule> emit() {
    if (mir_.files.size() != abi_.files.size()) {
      report(fallback_range(), "MIR and ABI file counts differ");
      return std::nullopt;
    }
    type_descriptor_globals_.resize(mir_.files.size());
    for (const AbiFileClass& file : abi_.files) {
      type_descriptor_globals_.at(file.file.value) =
          add_type_descriptor(file.file, file.type_descriptor);
    }
    for (std::size_t index = 0; index < mir_.files.size(); ++index) {
      emit_file(mir_.files[index], abi_.files[index]);
    }
    if (options_.emit_native_entry_point) {
      emit_native_entry_point();
    }
    if (!is_valid_) {
      return std::nullopt;
    }

    std::ostringstream output;
    output << "; Cloth LLVM IR\n"
           << "source_filename = \"cloth\"\n"
           << "target datalayout = \"" << abi_.target.llvm_data_layout << "\"\n"
           << "target triple = \"" << abi_.target.target_name << "\"\n\n"
           << "declare ptr @cloth_rt_alloc(ptr)\n"
           << "declare void @cloth_rt_gc_push_frame(ptr, ptr, i64)\n"
           << "declare void @cloth_rt_gc_pop_frame(ptr)\n"
           << "declare ptr @cloth_rt_string_literal(ptr, i64)\n"
           << "declare ptr @cloth_rt_string_concat(ptr, ptr)\n"
           << "declare i8 @cloth_rt_string_equal(ptr, ptr)\n"
           << "declare i32 @cloth_rt_string_length(ptr)\n"
           << "declare i32 @cloth_rt_string_byte_length(ptr)\n"
           << "declare i8 @cloth_rt_string_is_empty(ptr)\n"
           << "declare ptr @cloth_rt_object_type_name(ptr)\n"
           << "declare i8 @cloth_rt_object_is_kind(ptr, i64)\n"
           << "declare i8 @cloth_rt_object_is_type(ptr, ptr)\n"
           << "declare ptr @cloth_rt_array_alloc(i32, i64, i64, i8)\n"
           << "declare i32 @cloth_rt_array_length(ptr)\n"
           << "declare ptr @cloth_rt_array_element(ptr, i32)\n"
           << "declare void @cloth_rt_require_receiver(ptr)\n"
           << "declare void @cloth_rt_require_non_null(ptr)\n"
           << "declare void @cloth_rt_print(ptr)\n"
           << "declare void @cloth_rt_print_char(i32)\n"
           << "declare void @cloth_rt_print_i8(i8)\n"
           << "declare void @cloth_rt_print_i16(i16)\n"
           << "declare void @cloth_rt_print_i32(i32)\n"
           << "declare void @cloth_rt_print_i64(i64)\n"
           << "declare void @cloth_rt_print_u8(i8)\n"
           << "declare void @cloth_rt_print_u16(i16)\n"
           << "declare void @cloth_rt_print_u32(i32)\n"
           << "declare void @cloth_rt_print_u64(i64)\n"
           << "declare void @cloth_rt_print_f32(float)\n"
           << "declare void @cloth_rt_print_f64(double)\n"
           << "declare void @cloth_rt_print_bool(i8)\n"
           << "declare void @cloth_rt_print_object(ptr)\n"
           << "declare void @cloth_rt_print_newline()\n\n";
    for (const std::string& global : globals_) {
      output << global << '\n';
    }
    if (!globals_.empty()) {
      output << '\n';
    }
    output << definitions_.str();
    return LlvmIrModule{output.str()};
  }

  [[nodiscard]] const SemanticModel& semantics() const noexcept {
    return semantics_;
  }

  [[nodiscard]] const AbiModule& abi() const noexcept { return abi_; }

  [[nodiscard]] const MirFileClass& mir_file(FileId file) const {
    return mir_.files.at(file.value);
  }

  [[nodiscard]] std::string llvm_type(TypeId type) const {
    if (type.value >= abi_.types.size()) {
      return "void";
    }
    const AbiTypeLayout& layout = abi_.types[type.value];
    switch (layout.kind) {
      case AbiTypeKind::kInvalid:
      case AbiTypeKind::kVoid:
        return "void";
      case AbiTypeKind::kInteger:
        return "i" + std::to_string(layout.bit_width);
      case AbiTypeKind::kFloat:
        return layout.bit_width == 32 ? "float" : "double";
      case AbiTypeKind::kReference:
        return "ptr";
    }
    return "void";
  }

  [[nodiscard]] std::uint64_t alignment(TypeId type) const {
    return abi_.types.at(type.value).storage.alignment;
  }

  [[nodiscard]] bool is_reference(TypeId type) const {
    return type.value < abi_.types.size() &&
           abi_.types[type.value].kind == AbiTypeKind::kReference;
  }

  [[nodiscard]] std::uint64_t pointer_alignment() const noexcept {
    return abi_.target.pointer.alignment;
  }

  [[nodiscard]] std::uint64_t gc_frame_alignment() const noexcept {
    return std::max(abi_.target.pointer.alignment, abi_.target.int64_alignment);
  }

  [[nodiscard]] const AbiCallable* find_callable(SymbolId symbol) const {
    for (const AbiFileClass& file : abi_.files) {
      for (const AbiCallable& callable : file.functions) {
        if (callable.symbol == symbol) {
          return &callable;
        }
      }
      for (const AbiCallable& callable : file.constructors) {
        if (callable.symbol == symbol) {
          return &callable;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] const AbiFieldLayout* find_field(SymbolId symbol) const {
    for (const AbiFileClass& file : abi_.files) {
      for (const AbiFieldLayout& field : file.layout.fields) {
        if (field.symbol == symbol) {
          return &field;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] const AbiStaticField* find_static_field(SymbolId symbol) const {
    for (const AbiFileClass& file : abi_.files) {
      for (const AbiStaticField& field : file.static_fields) {
        if (field.symbol == symbol) {
          return &field;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] const MirField* find_mir_field(FileId file,
                                               SymbolId symbol) const {
    for (const MirField& field : mir_file(file).fields) {
      if (field.symbol == symbol) {
        return &field;
      }
    }
    return nullptr;
  }

  std::string add_string_literal(std::string value) {
    const std::size_t index = globals_.size();
    const std::string name = "@.cloth.str." + std::to_string(index);
    std::ostringstream global;
    global << name << " = private unnamed_addr constant [" << value.size()
           << " x i8] c\"" << llvm_string_bytes(value) << "\"";
    globals_.push_back(global.str());
    return name;
  }

  std::string add_type_descriptor(FileId file,
                                  const AbiTypeDescriptor& descriptor) {
    const std::string name_global = add_string_literal(descriptor.name);
    std::string references_global = "null";
    if (!descriptor.reference_offsets.empty()) {
      references_global =
          "@.cloth.type.references." + std::to_string(file.value);
      std::ostringstream references;
      references << references_global << " = private unnamed_addr constant ["
                 << descriptor.reference_offsets.size() << " x i64] [";
      for (std::size_t index = 0; index < descriptor.reference_offsets.size();
           ++index) {
        if (index != 0) {
          references << ", ";
        }
        references << "i64 " << descriptor.reference_offsets[index];
      }
      references << ']';
      globals_.push_back(references.str());
    }

    const std::string descriptor_global =
        "@.cloth.type." + std::to_string(file.value);
    std::ostringstream global;
    global << descriptor_global
           << " = private constant { i64, ptr, i64, i64, i64, ptr, i64 } "
              "{ i64 "
           << static_cast<std::uint64_t>(descriptor.kind) << ", ptr "
           << name_global << ", i64 " << descriptor.name.size() << ", i64 "
           << descriptor.size << ", i64 " << descriptor.alignment << ", ptr "
           << references_global << ", i64 "
           << descriptor.reference_offsets.size() << " }";
    globals_.push_back(global.str());
    return descriptor_global;
  }

  [[nodiscard]] const std::string& type_descriptor_global(FileId file) const {
    return type_descriptor_globals_.at(file.value);
  }

  void report(SourceRange range, std::string message) {
    diagnostics_.error(range, "LLVM lowering error: " + message);
    is_valid_ = false;
  }

 private:
  void emit_file(const MirFileClass& mir_file, const AbiFileClass& abi_file) {
    for (const MemberReference& member : abi_file.member_order) {
      switch (member.kind) {
        case DeclarationKind::kField: {
          const MirField& field = mir_file.fields.at(member.index);
          if (semantics_.symbol(field.symbol).is_static) {
            emit_static_field(field);
          } else if (field.initializer) {
            emit_field_initializer(abi_file, field.symbol, *field.initializer);
          }
          break;
        }
        case DeclarationKind::kFunction:
          emit_callable(abi_file, mir_file.functions.at(member.index),
                        abi_file.functions.at(member.index));
          break;
        case DeclarationKind::kConstructor:
          emit_callable(abi_file, mir_file.constructors.at(member.index),
                        abi_file.constructors.at(member.index));
          break;
        case DeclarationKind::kNestedType:
          break;
      }
    }
  }

  void emit_static_field(const MirField& field) {
    const AbiStaticField* abi_field = find_static_field(field.symbol);
    if (abi_field == nullptr || !field.initializer) {
      report(semantics_.symbol(field.symbol).range,
             "static field has no ABI declaration or initializer");
      return;
    }
    const MirInstruction* instruction =
        returned_instruction(*field.initializer);
    const auto* literal =
        instruction == nullptr
            ? nullptr
            : std::get_if<MirLiteralInstruction>(&instruction->data);
    if (literal == nullptr) {
      report(semantics_.symbol(field.symbol).range,
             "static field initializer is not a scalar literal");
      return;
    }
    const std::optional<std::string> value =
        lower_scalar_literal(*literal, abi_.types.at(abi_field->type.value));
    if (!value) {
      report(instruction->range, "static field literal is out of range");
      return;
    }
    std::ostringstream global;
    global << '@' << abi_field->mangled_name << " = ";
    if (abi_field->linkage == AbiLinkage::kInternal) {
      global << "internal ";
    }
    global << "constant " << llvm_type(abi_field->type) << ' ' << *value
           << ", align " << alignment(abi_field->type);
    globals_.push_back(global.str());
  }

  void emit_field_initializer(const AbiFileClass& file, SymbolId symbol,
                              const MirBody& body) {
    const AbiFieldLayout* field = find_field(symbol);
    if (field == nullptr) {
      report(semantics_.symbol(symbol).range,
             "instance field has no ABI layout");
      return;
    }
    const SemanticSymbol& class_symbol = semantics_.symbol(file.symbol);
    const SemanticSymbol& field_symbol = semantics_.symbol(field->symbol);
    const std::string name =
        field_initializer_name(class_symbol.name, field_symbol.name);
    definitions_ << "define internal " << llvm_type(field->type) << " @" << name
                 << "(ptr %receiver) {\n"
                 << BodyEmitter{*this,       body,  file,   field->type,
                                "%receiver", false, nullptr}
                        .emit()
                 << "}\n\n";
  }

  void emit_callable(const AbiFileClass& file, const MirCallable& mir_callable,
                     const AbiCallable& callable) {
    if (callable.linkage == AbiLinkage::kInternal) {
      definitions_ << "define internal ";
    } else {
      definitions_ << "define ";
    }
    definitions_ << llvm_type(callable.return_type) << " @"
                 << callable.mangled_name << '(';
    for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
      if (index != 0) {
        definitions_ << ", ";
      }
      const AbiParameter& parameter = callable.parameters[index];
      definitions_ << llvm_type(parameter.type) << ' ';
      if (parameter.kind == AbiParameterKind::kReceiver) {
        definitions_ << "%receiver";
      } else {
        definitions_ << "%arg" << parameter.symbol.value;
      }
    }
    definitions_ << ") {\n";
    const bool is_constructor = callable.kind == AbiCallableKind::kConstructor;
    const bool has_receiver =
        !callable.parameters.empty() &&
        callable.parameters.front().kind == AbiParameterKind::kReceiver;
    definitions_ << BodyEmitter{*this,
                                mir_callable.body,
                                file,
                                callable.return_type,
                                is_constructor
                                    ? "%self"
                                    : (has_receiver ? "%receiver" : "undef"),
                                is_constructor,
                                &callable}
                        .emit()
                 << "}\n\n";
  }

  void emit_native_entry_point() {
    const AbiCallable* entry = nullptr;
    SourceRange entry_range = fallback_range();
    bool saw_main = false;
    for (const AbiFileClass& file : abi_.files) {
      for (const AbiCallable& callable : file.functions) {
        const SemanticSymbol& symbol = semantics_.symbol(callable.symbol);
        if (symbol.name != "Main") {
          continue;
        }
        saw_main = true;
        entry_range = symbol.range;
        const TypeKind return_kind = semantics_.type(callable.return_type).kind;
        const bool valid_signature =
            symbol.is_static && symbol.parameter_types.empty() &&
            callable.linkage == AbiLinkage::kExternal &&
            (return_kind == TypeKind::kVoid || return_kind == TypeKind::kInt32);
        if (!valid_signature) {
          continue;
        }
        if (entry != nullptr) {
          report(symbol.range,
                 "native program has more than one eligible 'Main' function");
          return;
        }
        entry = &callable;
      }
    }
    if (entry == nullptr) {
      report(entry_range,
             saw_main
                 ? "entry point 'Main' must be public and static, take no "
                   "parameters, and return no value or int32"
                 : "native program requires a public static 'Main' function");
      return;
    }

    const TypeKind return_kind = semantics_.type(entry->return_type).kind;
    definitions_ << "define i32 @main() {\n"
                 << "entry:\n";
    if (return_kind == TypeKind::kVoid) {
      definitions_ << "  call void @" << entry->mangled_name << "()\n"
                   << "  ret i32 0\n";
    } else {
      definitions_ << "  %exit_code = call i32 @" << entry->mangled_name
                   << "()\n"
                   << "  ret i32 %exit_code\n";
    }
    definitions_ << "}\n\n";
  }

  static SourceRange fallback_range() noexcept {
    return point_range(SourceLocation{"<llvm>", 0, 1, 1});
  }

  const MirModule& mir_;
  const AbiModule& abi_;
  const SemanticModel& semantics_;
  DiagnosticEngine& diagnostics_;
  LlvmIrOptions options_;
  std::ostringstream definitions_;
  std::vector<std::string> globals_;
  std::vector<std::string> type_descriptor_globals_;
  bool is_valid_{true};
};

BodyEmitter::BodyEmitter(ModuleEmitter& module, const MirBody& body,
                         const AbiFileClass& file, TypeId return_type,
                         std::string receiver, bool is_constructor,
                         const AbiCallable* callable)
    : module_(module),
      body_(body),
      file_(file),
      return_type_(return_type),
      receiver_(std::move(receiver)),
      is_constructor_(is_constructor),
      callable_(callable),
      values_(body.value_count),
      value_types_(body.value_count, module.semantics().error_type()) {
  prepare_values();
  collect_storage();
  collect_gc_roots();
  analyze_gc_liveness();
}

std::string BodyEmitter::emit() {
  std::ostringstream output;
  for (std::size_t block_index = 0; block_index < body_.blocks.size();
       ++block_index) {
    const MirBasicBlock& block = body_.blocks[block_index];
    output << "bb" << block_index << ":\n";
    for (const MirInstruction& instruction : block.instructions) {
      if (const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data)) {
        emit_phi(instruction, *phi, output);
      }
    }
    if (block_index == body_.entry.value) {
      emit_prologue(output);
    }
    for (const MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<MirPhiInstruction>(instruction.data)) {
        emit_gc_value_root(instruction, output);
      }
    }
    emit_gc_block_entry_clears(block_index, output);
    for (std::size_t instruction_index = 0;
         instruction_index < block.instructions.size(); ++instruction_index) {
      const MirInstruction& instruction = block.instructions[instruction_index];
      if (!std::holds_alternative<MirPhiInstruction>(instruction.data)) {
        emit_instruction(instruction, output);
        emit_gc_value_root(instruction, output);
        emit_gc_dead_roots(block_index, instruction_index, instruction, output);
      }
    }
    emit_terminator(block.terminator, block.is_reachable, output);
  }
  return output.str();
}

void BodyEmitter::prepare_values() {
  for (const MirBasicBlock& block : body_.blocks) {
    for (const MirInstruction& instruction : block.instructions) {
      if (instruction.result) {
        const std::size_t index = instruction.result->value;
        if (index >= values_.size()) {
          module_.report(instruction.range, "MIR value exceeds its body table");
          continue;
        }
        values_[index] = "%v" + std::to_string(index);
        value_types_[index] = instruction.type;
      }
    }
  }
}

void BodyEmitter::collect_storage() {
  if (callable_ != nullptr) {
    for (const AbiParameter& parameter : callable_->parameters) {
      if (parameter.kind == AbiParameterKind::kExplicit) {
        storage_symbols_.push_back(parameter.symbol);
      }
    }
  }
  for (const MirBasicBlock& block : body_.blocks) {
    for (const MirInstruction& instruction : block.instructions) {
      if (const auto* declaration =
              std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
        if (std::find(storage_symbols_.begin(), storage_symbols_.end(),
                      declaration->symbol) == storage_symbols_.end()) {
          storage_symbols_.push_back(declaration->symbol);
        }
      }
    }
  }
}

void BodyEmitter::collect_gc_roots() {
  const SemanticModel& semantics = module_.semantics();
  for (const SymbolId symbol : storage_symbols_) {
    if (module_.is_reference(semantics.symbol(symbol).type)) {
      gc_symbol_roots_.push_back(symbol);
    }
  }

  gc_value_roots_.resize(value_types_.size(), false);
  for (const MirBasicBlock& block : body_.blocks) {
    if (!block.is_reachable) {
      continue;
    }
    for (const MirInstruction& instruction : block.instructions) {
      if (instruction.result &&
          instruction.result->value < gc_value_roots_.size() &&
          !gc_value_roots_[instruction.result->value] &&
          module_.is_reference(instruction.type)) {
        gc_value_roots_[instruction.result->value] = true;
        ++gc_value_root_count_;
      }
    }
  }
}

void BodyEmitter::analyze_gc_liveness() {
  const std::size_t block_count = body_.blocks.size();
  const std::size_t value_count = gc_value_roots_.size();
  const std::size_t symbol_count = module_.semantics().symbols().size();
  const auto empty_set = [value_count, symbol_count] {
    return GcLiveSet{std::vector<bool>(value_count, false),
                     std::vector<bool>(symbol_count, false)};
  };
  const auto add_value = [this](GcLiveSet& set, MirValueId value) {
    if (value.value < gc_value_roots_.size() && gc_value_roots_[value.value]) {
      set.values[value.value] = true;
    }
  };
  const auto add_symbol = [this](GcLiveSet& set, SymbolId symbol) {
    if (has_gc_symbol_root(symbol)) {
      set.symbols[symbol.value] = true;
    }
  };
  const auto merge = [](GcLiveSet& destination, const GcLiveSet& source) {
    for (std::size_t index = 0; index < destination.values.size(); ++index) {
      destination.values[index] =
          destination.values[index] || source.values[index];
    }
    for (std::size_t index = 0; index < destination.symbols.size(); ++index) {
      destination.symbols[index] =
          destination.symbols[index] || source.symbols[index];
    }
  };

  std::vector<GcLiveSet> block_uses(block_count);
  std::vector<GcLiveSet> block_definitions(block_count);
  gc_live_out_.resize(block_count);
  gc_live_in_.resize(block_count);
  for (std::size_t block_index = 0; block_index < block_count; ++block_index) {
    block_uses[block_index] = empty_set();
    block_definitions[block_index] = empty_set();
    gc_live_out_[block_index] = empty_set();
    gc_live_in_[block_index] = empty_set();
    const MirBasicBlock& block = body_.blocks[block_index];
    if (!block.is_reachable) {
      continue;
    }
    for (const MirInstruction& instruction : block.instructions) {
      if (!std::holds_alternative<MirPhiInstruction>(instruction.data)) {
        for (const MirValueId use : instruction_value_uses(instruction)) {
          if (use.value < value_count && gc_value_roots_[use.value] &&
              !block_definitions[block_index].values[use.value]) {
            block_uses[block_index].values[use.value] = true;
          }
        }
      }
      if (const auto* load =
              std::get_if<MirLoadSymbolInstruction>(&instruction.data);
          load != nullptr && has_gc_symbol_root(load->symbol) &&
          !block_definitions[block_index].symbols[load->symbol.value]) {
        block_uses[block_index].symbols[load->symbol.value] = true;
      }
      if (const auto* declaration =
              std::get_if<MirDeclareLocalInstruction>(&instruction.data);
          declaration != nullptr && has_gc_symbol_root(declaration->symbol)) {
        block_definitions[block_index].symbols[declaration->symbol.value] =
            true;
      } else if (const auto* store =
                     std::get_if<MirStoreSymbolInstruction>(&instruction.data);
                 store != nullptr && has_gc_symbol_root(store->symbol)) {
        block_definitions[block_index].symbols[store->symbol.value] = true;
      }
      if (instruction.result &&
          instruction.result->value < gc_value_roots_.size() &&
          gc_value_roots_[instruction.result->value]) {
        block_definitions[block_index].values[instruction.result->value] = true;
      }
    }
    for (const MirValueId use : terminator_value_uses(block.terminator)) {
      if (use.value < value_count && gc_value_roots_[use.value] &&
          !block_definitions[block_index].values[use.value]) {
        block_uses[block_index].values[use.value] = true;
      }
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t reverse_index = block_count; reverse_index > 0;
         --reverse_index) {
      const std::size_t block_index = reverse_index - 1;
      const MirBasicBlock& block = body_.blocks[block_index];
      if (!block.is_reachable) {
        continue;
      }
      GcLiveSet next_out = empty_set();
      for (const MirBlockId successor :
           terminator_successors(block.terminator)) {
        if (successor.value >= block_count ||
            !body_.blocks[successor.value].is_reachable) {
          continue;
        }
        merge(next_out, gc_live_in_[successor.value]);
        for (const MirInstruction& instruction :
             body_.blocks[successor.value].instructions) {
          const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data);
          if (phi == nullptr) {
            break;
          }
          for (const MirPhiIncoming& incoming : phi->incoming) {
            if (incoming.predecessor.value == block_index) {
              add_value(next_out, incoming.value);
            }
          }
        }
      }

      GcLiveSet next_in = block_uses[block_index];
      for (std::size_t index = 0; index < value_count; ++index) {
        if (next_out.values[index] &&
            !block_definitions[block_index].values[index]) {
          next_in.values[index] = true;
        }
      }
      for (std::size_t index = 0; index < symbol_count; ++index) {
        if (next_out.symbols[index] &&
            !block_definitions[block_index].symbols[index]) {
          next_in.symbols[index] = true;
        }
      }
      if (!(next_out == gc_live_out_[block_index]) ||
          !(next_in == gc_live_in_[block_index])) {
        gc_live_out_[block_index] = std::move(next_out);
        gc_live_in_[block_index] = std::move(next_in);
        changed = true;
      }
    }
  }

  gc_live_after_phis_.resize(block_count);
  gc_live_after_instructions_.resize(block_count);
  for (std::size_t block_index = 0; block_index < block_count; ++block_index) {
    const MirBasicBlock& block = body_.blocks[block_index];
    GcLiveSet live = gc_live_out_[block_index];
    for (const MirValueId use : terminator_value_uses(block.terminator)) {
      add_value(live, use);
    }
    auto& instruction_states = gc_live_after_instructions_[block_index];
    instruction_states.resize(block.instructions.size());
    for (std::size_t reverse_index = block.instructions.size();
         reverse_index > 0; --reverse_index) {
      const std::size_t instruction_index = reverse_index - 1;
      const MirInstruction& instruction = block.instructions[instruction_index];
      instruction_states[instruction_index] = live;
      if (instruction.result &&
          instruction.result->value < gc_value_roots_.size() &&
          gc_value_roots_[instruction.result->value]) {
        live.values[instruction.result->value] = false;
      }
      if (!std::holds_alternative<MirPhiInstruction>(instruction.data)) {
        for (const MirValueId use : instruction_value_uses(instruction)) {
          add_value(live, use);
        }
      }
      if (const auto* declaration =
              std::get_if<MirDeclareLocalInstruction>(&instruction.data);
          declaration != nullptr && has_gc_symbol_root(declaration->symbol)) {
        live.symbols[declaration->symbol.value] = false;
      } else if (const auto* store =
                     std::get_if<MirStoreSymbolInstruction>(&instruction.data);
                 store != nullptr && has_gc_symbol_root(store->symbol)) {
        live.symbols[store->symbol.value] = false;
      }
      if (const auto* load =
              std::get_if<MirLoadSymbolInstruction>(&instruction.data);
          load != nullptr) {
        add_symbol(live, load->symbol);
      }
    }

    std::size_t phi_count = 0;
    while (phi_count < block.instructions.size() &&
           std::holds_alternative<MirPhiInstruction>(
               block.instructions[phi_count].data)) {
      ++phi_count;
    }
    gc_live_after_phis_[block_index] = phi_count == 0
                                           ? gc_live_in_[block_index]
                                           : instruction_states[phi_count - 1];
  }
}

void BodyEmitter::emit_prologue(std::ostringstream& output) {
  const SemanticModel& semantics = module_.semantics();
  const std::uint64_t pointer_alignment = module_.pointer_alignment();
  for (const SymbolId symbol : storage_symbols_) {
    const TypeId type = semantics.symbol(symbol).type;
    output << "  " << symbol_address(symbol) << " = alloca "
           << module_.llvm_type(type) << ", align " << module_.alignment(type)
           << '\n';
  }
  for (std::size_t index = 0; index < gc_value_roots_.size(); ++index) {
    if (gc_value_roots_[index]) {
      output << "  " << gc_value_address(MirValueId{index})
             << " = alloca ptr, align " << pointer_alignment << '\n';
    }
  }
  if (has_receiver_root()) {
    output << "  %gc.receiver = alloca ptr, align " << pointer_alignment
           << '\n';
  }
  if (is_constructor_) {
    output << "  %gc.self = alloca ptr, align " << pointer_alignment << '\n';
  }
  const std::size_t root_count = gc_root_count();
  if (root_count != 0) {
    output << "  %gc.frame = alloca { ptr, ptr, i64 }, align "
           << module_.gc_frame_alignment() << '\n'
           << "  %gc.roots = alloca [" << root_count << " x ptr], align "
           << pointer_alignment << '\n';
  }

  for (const SymbolId symbol : gc_symbol_roots_) {
    output << "  store ptr null, ptr " << symbol_address(symbol) << ", align "
           << pointer_alignment << '\n';
  }
  for (std::size_t index = 0; index < gc_value_roots_.size(); ++index) {
    if (gc_value_roots_[index]) {
      output << "  store ptr null, ptr " << gc_value_address(MirValueId{index})
             << ", align " << pointer_alignment << '\n';
    }
  }
  if (is_constructor_) {
    output << "  store ptr null, ptr %gc.self, align " << pointer_alignment
           << '\n';
  }
  if (callable_ != nullptr) {
    for (const AbiParameter& parameter : callable_->parameters) {
      if (parameter.kind == AbiParameterKind::kExplicit) {
        output << "  store " << module_.llvm_type(parameter.type) << ' '
               << argument_name(parameter.symbol) << ", ptr "
               << symbol_address(parameter.symbol) << ", align "
               << module_.alignment(parameter.type) << '\n';
      }
    }
  }
  if (has_receiver_root()) {
    output << "  store ptr " << receiver_ << ", ptr %gc.receiver, align "
           << pointer_alignment << '\n';
  }
  if (root_count != 0) {
    std::vector<std::string> root_slots;
    root_slots.reserve(root_count);
    if (has_receiver_root()) {
      root_slots.emplace_back("%gc.receiver");
    }
    if (is_constructor_) {
      root_slots.emplace_back("%gc.self");
    }
    for (const SymbolId symbol : gc_symbol_roots_) {
      root_slots.push_back(symbol_address(symbol));
    }
    for (std::size_t index = 0; index < gc_value_roots_.size(); ++index) {
      if (gc_value_roots_[index]) {
        root_slots.push_back(gc_value_address(MirValueId{index}));
      }
    }
    for (std::size_t index = 0; index < root_slots.size(); ++index) {
      output << "  %gc.root." << index << " = getelementptr [" << root_count
             << " x ptr], ptr %gc.roots, i64 0, i64 " << index << '\n'
             << "  store ptr " << root_slots[index] << ", ptr %gc.root."
             << index << ", align " << pointer_alignment << '\n';
    }
    output << "  call void @cloth_rt_gc_push_frame(ptr %gc.frame, ptr "
              "%gc.roots, i64 "
           << root_count << ")\n";
  }
  if (is_constructor_) {
    output << "  %self = call ptr @cloth_rt_alloc(ptr "
           << module_.type_descriptor_global(file_.file) << ")\n"
           << "  store ptr %self, ptr %gc.self, align " << pointer_alignment
           << '\n';
    emit_field_initializers(output);
  }
}

void BodyEmitter::emit_gc_value_root(const MirInstruction& instruction,
                                     std::ostringstream& output) const {
  if (!instruction.result ||
      instruction.result->value >= gc_value_roots_.size() ||
      !gc_value_roots_[instruction.result->value]) {
    return;
  }
  output << "  store ptr " << value(*instruction.result) << ", ptr "
         << gc_value_address(*instruction.result) << ", align "
         << module_.pointer_alignment() << '\n';
}

void BodyEmitter::emit_gc_block_entry_clears(std::size_t block,
                                             std::ostringstream& output) const {
  if (block >= gc_live_after_phis_.size()) {
    return;
  }
  const GcLiveSet& live = gc_live_after_phis_[block];
  GcLiveSet candidates{
      std::vector<bool>(gc_value_roots_.size(), false),
      std::vector<bool>(module_.semantics().symbols().size(), false)};
  if (block == body_.entry.value && callable_ != nullptr) {
    for (const AbiParameter& parameter : callable_->parameters) {
      if (parameter.kind == AbiParameterKind::kExplicit &&
          has_gc_symbol_root(parameter.symbol)) {
        candidates.symbols[parameter.symbol.value] = true;
      }
    }
  }
  for (std::size_t predecessor = 0; predecessor < body_.blocks.size();
       ++predecessor) {
    for (const MirBlockId successor :
         terminator_successors(body_.blocks[predecessor].terminator)) {
      if (successor.value != block || predecessor >= gc_live_out_.size()) {
        continue;
      }
      const GcLiveSet& predecessor_live = gc_live_out_[predecessor];
      for (std::size_t index = 0; index < candidates.values.size(); ++index) {
        candidates.values[index] =
            candidates.values[index] || predecessor_live.values[index];
      }
      for (std::size_t index = 0; index < candidates.symbols.size(); ++index) {
        candidates.symbols[index] =
            candidates.symbols[index] || predecessor_live.symbols[index];
      }
    }
  }
  const std::uint64_t alignment = module_.pointer_alignment();
  for (const SymbolId symbol : gc_symbol_roots_) {
    if (candidates.symbols[symbol.value] && !live.symbols[symbol.value]) {
      output << "  store ptr null, ptr " << symbol_address(symbol) << ", align "
             << alignment << '\n';
    }
  }
  for (std::size_t index = 0; index < gc_value_roots_.size(); ++index) {
    if (candidates.values[index] && !live.values[index]) {
      output << "  store ptr null, ptr " << gc_value_address(MirValueId{index})
             << ", align " << alignment << '\n';
    }
  }
}

void BodyEmitter::emit_gc_dead_roots(std::size_t block,
                                     std::size_t instruction_index,
                                     const MirInstruction& instruction,
                                     std::ostringstream& output) const {
  if (block >= gc_live_after_instructions_.size() ||
      instruction_index >= gc_live_after_instructions_[block].size()) {
    return;
  }
  const GcLiveSet& live = gc_live_after_instructions_[block][instruction_index];
  std::vector<bool> clear_values(gc_value_roots_.size(), false);
  const auto request_value_clear = [this, &live,
                                    &clear_values](MirValueId value) {
    if (value.value < gc_value_roots_.size() && gc_value_roots_[value.value] &&
        !live.values[value.value]) {
      clear_values[value.value] = true;
    }
  };
  for (const MirValueId use : instruction_value_uses(instruction)) {
    request_value_clear(use);
  }
  if (instruction.result) {
    request_value_clear(*instruction.result);
  }

  const std::uint64_t alignment = module_.pointer_alignment();
  for (std::size_t index = 0; index < clear_values.size(); ++index) {
    if (clear_values[index]) {
      output << "  store ptr null, ptr " << gc_value_address(MirValueId{index})
             << ", align " << alignment << '\n';
    }
  }

  std::optional<SymbolId> touched_symbol;
  if (const auto* load =
          std::get_if<MirLoadSymbolInstruction>(&instruction.data)) {
    touched_symbol = load->symbol;
  } else if (const auto* declaration =
                 std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
    touched_symbol = declaration->symbol;
  } else if (const auto* store =
                 std::get_if<MirStoreSymbolInstruction>(&instruction.data)) {
    touched_symbol = store->symbol;
  }
  if (touched_symbol && has_gc_symbol_root(*touched_symbol) &&
      !live.symbols[touched_symbol->value]) {
    output << "  store ptr null, ptr " << symbol_address(*touched_symbol)
           << ", align " << alignment << '\n';
  }
}

void BodyEmitter::emit_gc_epilogue(std::ostringstream& output) const {
  if (gc_root_count() != 0) {
    output << "  call void @cloth_rt_gc_pop_frame(ptr %gc.frame)\n";
  }
}

void BodyEmitter::emit_field_initializers(std::ostringstream& output) {
  const SemanticModel& semantics = module_.semantics();
  const SemanticSymbol& class_symbol = semantics.symbol(file_.symbol);
  for (std::size_t index = 0; index < file_.layout.fields.size(); ++index) {
    const AbiFieldLayout& field = file_.layout.fields[index];
    const SemanticSymbol& field_symbol = semantics.symbol(field.symbol);
    const std::string initializer =
        field_initializer_name(class_symbol.name, field_symbol.name);
    // Initializer presence is encoded by a generated helper. The module emitter
    // exposes it only for fields whose MIR contains an initializer.
    const MirField* mir_field =
        module_.find_mir_field(file_.file, field.symbol);
    if (mir_field == nullptr || !mir_field->initializer) {
      continue;
    }
    const std::string value_name = "%init" + std::to_string(index);
    const std::string address = "%init.addr" + std::to_string(index);
    output << "  " << value_name << " = call " << module_.llvm_type(field.type)
           << " @" << initializer << "(ptr %self)\n"
           << "  " << address << " = getelementptr i8, ptr %self, i64 "
           << field.offset << '\n'
           << "  store " << module_.llvm_type(field.type) << ' ' << value_name
           << ", ptr " << address << ", align " << module_.alignment(field.type)
           << '\n';
  }
}

void BodyEmitter::emit_instruction(const MirInstruction& instruction,
                                   std::ostringstream& output) {
  if (const auto* literal =
          std::get_if<MirLiteralInstruction>(&instruction.data)) {
    emit_literal(instruction, *literal, output);
  } else if (const auto* load =
                 std::get_if<MirLoadSymbolInstruction>(&instruction.data)) {
    emit_load_symbol(instruction, *load, output);
  } else if (const auto* declaration =
                 std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
    if (declaration->initializer) {
      const TypeId type = module_.semantics().symbol(declaration->symbol).type;
      output << "  store " << module_.llvm_type(type) << ' '
             << value(*declaration->initializer) << ", ptr "
             << symbol_address(declaration->symbol) << ", align "
             << module_.alignment(type) << '\n';
    }
  } else if (const auto* store =
                 std::get_if<MirStoreSymbolInstruction>(&instruction.data)) {
    const TypeId type = module_.semantics().symbol(store->symbol).type;
    output << "  store " << module_.llvm_type(type) << ' '
           << value(store->value) << ", ptr " << symbol_address(store->symbol)
           << ", align " << module_.alignment(type) << '\n';
  } else if (const auto* load =
                 std::get_if<MirLoadMemberInstruction>(&instruction.data)) {
    emit_member_load(instruction, *load, output);
  } else if (const auto* store =
                 std::get_if<MirStoreMemberInstruction>(&instruction.data)) {
    emit_member_store(instruction, *store, output);
  } else if (const auto* array =
                 std::get_if<MirArrayLiteralInstruction>(&instruction.data)) {
    emit_array_literal(instruction, *array, output);
  } else if (const auto* load =
                 std::get_if<MirArrayLoadInstruction>(&instruction.data)) {
    emit_array_load(instruction, *load, output);
  } else if (const auto* store =
                 std::get_if<MirArrayStoreInstruction>(&instruction.data)) {
    emit_array_store(instruction, *store, output);
  } else if (const auto* length =
                 std::get_if<MirArrayLengthInstruction>(&instruction.data)) {
    emit_array_length(instruction, *length, output);
  } else if (const auto* meta =
                 std::get_if<MirStringMetaInstruction>(&instruction.data)) {
    emit_string_meta(instruction, *meta, output);
  } else if (const auto* meta =
                 std::get_if<MirObjectMetaInstruction>(&instruction.data)) {
    emit_object_meta(instruction, *meta, output);
  } else if (const auto* unary =
                 std::get_if<MirUnaryInstruction>(&instruction.data)) {
    emit_unary(instruction, *unary, output);
  } else if (const auto* binary =
                 std::get_if<MirBinaryInstruction>(&instruction.data)) {
    emit_binary(instruction, *binary, output);
  } else if (const auto* conversion =
                 std::get_if<MirConvertInstruction>(&instruction.data)) {
    values_.at(instruction.result->value) = value(conversion->value);
  } else if (const auto* test =
                 std::get_if<MirIsNonNullInstruction>(&instruction.data)) {
    emit_is_non_null(instruction, *test, output);
  } else if (const auto* assertion =
                 std::get_if<MirNullAssertInstruction>(&instruction.data)) {
    emit_null_assert(instruction, *assertion, output);
  } else if (const auto* test =
                 std::get_if<MirTypeTestInstruction>(&instruction.data)) {
    emit_type_test(instruction, *test, output);
  } else if (const auto* cast =
                 std::get_if<MirCheckedCastInstruction>(&instruction.data)) {
    emit_checked_cast(instruction, *cast, output);
  } else if (const auto* call =
                 std::get_if<MirCallInstruction>(&instruction.data)) {
    emit_call(instruction, *call, output);
  } else if (std::holds_alternative<MirInvalidInstruction>(instruction.data)) {
    module_.report(instruction.range, "invalid MIR reached LLVM lowering");
  }
}

void BodyEmitter::emit_literal(const MirInstruction& instruction,
                               const MirLiteralInstruction& literal,
                               std::ostringstream& output) {
  std::string lowered;
  switch (literal.kind) {
    case LiteralKind::kInteger:
    case LiteralKind::kFloat:
    case LiteralKind::kCharacter:
    case LiteralKind::kBoolean: {
      const std::optional<std::string> value = lower_scalar_literal(
          literal, module_.abi().types.at(instruction.type.value));
      if (!value) {
        module_.report(instruction.range,
                       literal.kind == LiteralKind::kInteger
                           ? "integer literal is out of range"
                           : "floating literal is out of range");
        lowered = "0";
      } else {
        lowered = *value;
      }
      break;
    }
    case LiteralKind::kString: {
      const std::string decoded = decode_string_literal(literal.lexeme);
      const std::string global = module_.add_string_literal(decoded);
      output << "  " << result_name(instruction)
             << " = call ptr @cloth_rt_string_literal(ptr " << global
             << ", i64 " << decoded.size() << ")\n";
      return;
    }
    case LiteralKind::kNull:
      lowered = "null";
      break;
  }
  values_.at(instruction.result->value) = std::move(lowered);
}

void BodyEmitter::emit_load_symbol(const MirInstruction& instruction,
                                   const MirLoadSymbolInstruction& load,
                                   std::ostringstream& output) {
  const SemanticModel& semantics = module_.semantics();
  if (load.symbol == semantics.file(file_.file).self_symbol) {
    values_.at(instruction.result->value) = receiver_;
    return;
  }
  const TypeId type = semantics.symbol(load.symbol).type;
  output << "  " << result_name(instruction) << " = load "
         << module_.llvm_type(type) << ", ptr " << symbol_address(load.symbol)
         << ", align " << module_.alignment(type) << '\n';
}

void BodyEmitter::emit_member_load(const MirInstruction& instruction,
                                   const MirLoadMemberInstruction& load,
                                   std::ostringstream& output) {
  const AbiFieldLayout* field = module_.find_field(load.member);
  if (field == nullptr) {
    module_.report(instruction.range, "member load has no ABI field");
    return;
  }
  const std::string object = value(load.object);
  const std::string address = next_address();
  output << "  call void @cloth_rt_require_receiver(ptr " << object << ")\n"
         << "  " << address << " = getelementptr i8, ptr " << object << ", i64 "
         << field->offset << '\n'
         << "  " << result_name(instruction) << " = load "
         << module_.llvm_type(field->type) << ", ptr " << address << ", align "
         << module_.alignment(field->type) << '\n';
}

void BodyEmitter::emit_member_store(const MirInstruction& instruction,
                                    const MirStoreMemberInstruction& store,
                                    std::ostringstream& output) {
  const AbiFieldLayout* field = module_.find_field(store.member);
  if (field == nullptr) {
    module_.report(instruction.range, "member store has no ABI field");
    return;
  }
  const std::string object = value(store.object);
  const std::string address = next_address();
  output << "  call void @cloth_rt_require_receiver(ptr " << object << ")\n"
         << "  " << address << " = getelementptr i8, ptr " << object << ", i64 "
         << field->offset << '\n'
         << "  store " << module_.llvm_type(field->type) << ' '
         << value(store.value) << ", ptr " << address << ", align "
         << module_.alignment(field->type) << '\n';
}

void BodyEmitter::emit_array_literal(const MirInstruction& instruction,
                                     const MirArrayLiteralInstruction& array,
                                     std::ostringstream& output) {
  if (array.elements.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    module_.report(instruction.range, "array literal has too many elements");
    return;
  }
  const AbiTypeLayout& element =
      module_.abi().types.at(array.element_type.value);
  const std::uint8_t contains_references =
      element.kind == AbiTypeKind::kReference ? 1 : 0;
  output << "  " << result_name(instruction)
         << " = call ptr @cloth_rt_array_alloc(i32 " << array.elements.size()
         << ", i64 " << element.storage.size << ", i64 "
         << element.storage.alignment << ", i8 "
         << static_cast<unsigned int>(contains_references) << ")\n";
  for (std::size_t index = 0; index < array.elements.size(); ++index) {
    const std::string address = next_address();
    output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
           << result_name(instruction) << ", i32 " << index << ")\n"
           << "  store " << module_.llvm_type(array.element_type) << ' '
           << value(array.elements[index]) << ", ptr " << address << ", align "
           << element.storage.alignment << '\n';
  }
}

void BodyEmitter::emit_array_load(const MirInstruction& instruction,
                                  const MirArrayLoadInstruction& load,
                                  std::ostringstream& output) {
  const std::string address = next_address();
  output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
         << value(load.array) << ", i32 " << value(load.index) << ")\n"
         << "  " << result_name(instruction) << " = load "
         << module_.llvm_type(instruction.type) << ", ptr " << address
         << ", align " << module_.alignment(instruction.type) << '\n';
}

void BodyEmitter::emit_array_store(const MirInstruction&,
                                   const MirArrayStoreInstruction& store,
                                   std::ostringstream& output) {
  const TypeId type = value_type(store.value);
  const std::string address = next_address();
  output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
         << value(store.array) << ", i32 " << value(store.index) << ")\n"
         << "  store " << module_.llvm_type(type) << ' ' << value(store.value)
         << ", ptr " << address << ", align " << module_.alignment(type)
         << '\n';
}

void BodyEmitter::emit_array_length(const MirInstruction& instruction,
                                    const MirArrayLengthInstruction& length,
                                    std::ostringstream& output) {
  output << "  " << result_name(instruction)
         << " = call i32 @cloth_rt_array_length(ptr " << value(length.array)
         << ")\n";
}

void BodyEmitter::emit_string_meta(const MirInstruction& instruction,
                                   const MirStringMetaInstruction& meta,
                                   std::ostringstream& output) {
  const std::string operand = value(meta.string);
  switch (meta.query) {
    case StringMetaQuery::kLength:
      output << "  " << result_name(instruction)
             << " = call i32 @cloth_rt_string_length(ptr " << operand << ")\n";
      break;
    case StringMetaQuery::kByteLength:
      output << "  " << result_name(instruction)
             << " = call i32 @cloth_rt_string_byte_length(ptr " << operand
             << ")\n";
      break;
    case StringMetaQuery::kIsEmpty: {
      const std::string raw_result = next_address();
      output << "  " << raw_result
             << " = call i8 @cloth_rt_string_is_empty(ptr " << operand << ")\n"
             << "  " << result_name(instruction) << " = icmp ne i8 "
             << raw_result << ", 0\n";
      break;
    }
  }
}

void BodyEmitter::emit_object_meta(const MirInstruction& instruction,
                                   const MirObjectMetaInstruction& meta,
                                   std::ostringstream& output) {
  output << "  " << result_name(instruction)
         << " = call ptr @cloth_rt_object_type_name(ptr " << value(meta.object)
         << ")\n";
}

void BodyEmitter::emit_unary(const MirInstruction& instruction,
                             const MirUnaryInstruction& unary,
                             std::ostringstream& output) {
  const TypeId operand_type = value_type(unary.operand);
  const TypeKind kind = module_.semantics().type(operand_type).kind;
  const std::string type = module_.llvm_type(operand_type);
  const std::string operand = value(unary.operand);
  if (unary.operation == TokenKind::kPlus) {
    values_.at(instruction.result->value) = operand;
    return;
  }
  output << "  " << result_name(instruction) << " = ";
  if (unary.operation == TokenKind::kBang) {
    output << "xor i1 " << operand << ", true\n";
  } else if (unary.operation == TokenKind::kTilde) {
    output << "xor " << type << ' ' << operand << ", -1\n";
  } else if (kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64) {
    output << "fneg " << type << ' ' << operand << '\n';
  } else {
    output << "sub " << type << " 0, " << operand << '\n';
  }
}

void BodyEmitter::emit_binary(const MirInstruction& instruction,
                              const MirBinaryInstruction& binary,
                              std::ostringstream& output) {
  const TypeId operand_type = value_type(binary.left);
  const TypeKind kind = module_.semantics().type(operand_type).kind;
  if (binary.operation == TokenKind::kPlus &&
      operand_type == module_.semantics().string_type() &&
      value_type(binary.right) == module_.semantics().string_type()) {
    output << "  " << result_name(instruction)
           << " = call ptr @cloth_rt_string_concat(ptr " << value(binary.left)
           << ", ptr " << value(binary.right) << ")\n";
    return;
  }
  if ((binary.operation == TokenKind::kEqualEqual ||
       binary.operation == TokenKind::kBangEqual) &&
      is_string_like(operand_type) &&
      is_string_like(value_type(binary.right))) {
    const std::string equal = next_address();
    output << "  " << equal << " = call i8 @cloth_rt_string_equal(ptr "
           << value(binary.left) << ", ptr " << value(binary.right) << ")\n"
           << "  " << result_name(instruction) << " = icmp "
           << (binary.operation == TokenKind::kEqualEqual ? "ne" : "eq")
           << " i8 " << equal << ", 0\n";
    return;
  }
  const bool is_float =
      kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64;
  const bool is_unsigned =
      kind == TypeKind::kByte || kind == TypeKind::kChar ||
      kind == TypeKind::kUint8 || kind == TypeKind::kUint16 ||
      kind == TypeKind::kUint32 || kind == TypeKind::kUint64;
  const std::string type = module_.llvm_type(operand_type);
  std::string operation;
  switch (binary.operation) {
    case TokenKind::kPlus:
      operation = is_float ? "fadd" : "add";
      break;
    case TokenKind::kMinus:
      operation = is_float ? "fsub" : "sub";
      break;
    case TokenKind::kStar:
      operation = is_float ? "fmul" : "mul";
      break;
    case TokenKind::kSlash:
      operation = is_float ? "fdiv" : (is_unsigned ? "udiv" : "sdiv");
      break;
    case TokenKind::kPercent:
      operation = is_float ? "frem" : (is_unsigned ? "urem" : "srem");
      break;
    case TokenKind::kEqualEqual:
      operation = is_float ? "fcmp oeq" : "icmp eq";
      break;
    case TokenKind::kBangEqual:
      operation = is_float ? "fcmp une" : "icmp ne";
      break;
    case TokenKind::kLess:
      operation =
          is_float ? "fcmp olt" : (is_unsigned ? "icmp ult" : "icmp slt");
      break;
    case TokenKind::kLessEqual:
      operation =
          is_float ? "fcmp ole" : (is_unsigned ? "icmp ule" : "icmp sle");
      break;
    case TokenKind::kGreater:
      operation =
          is_float ? "fcmp ogt" : (is_unsigned ? "icmp ugt" : "icmp sgt");
      break;
    case TokenKind::kGreaterEqual:
      operation =
          is_float ? "fcmp oge" : (is_unsigned ? "icmp uge" : "icmp sge");
      break;
    default:
      module_.report(instruction.range,
                     "unsupported binary operator reached LLVM lowering");
      return;
  }
  output << "  " << result_name(instruction) << " = " << operation << ' '
         << type << ' ' << value(binary.left) << ", " << value(binary.right)
         << '\n';
}

void BodyEmitter::emit_is_non_null(const MirInstruction& instruction,
                                   const MirIsNonNullInstruction& test,
                                   std::ostringstream& output) {
  output << "  " << result_name(instruction) << " = icmp ne ptr "
         << value(test.value) << ", null\n";
}

void BodyEmitter::emit_null_assert(const MirInstruction& instruction,
                                   const MirNullAssertInstruction& assertion,
                                   std::ostringstream& output) {
  const std::string operand = value(assertion.value);
  output << "  call void @cloth_rt_require_non_null(ptr " << operand << ")\n";
  values_.at(instruction.result->value) = operand;
}

void BodyEmitter::emit_type_test(const MirInstruction& instruction,
                                 const MirTypeTestInstruction& test,
                                 std::ostringstream& output) {
  emit_type_condition(result_name(instruction), test.value, test.target,
                      instruction.range, output);
}

void BodyEmitter::emit_checked_cast(const MirInstruction& instruction,
                                    const MirCheckedCastInstruction& cast,
                                    std::ostringstream& output) {
  const std::string condition = next_address();
  emit_type_condition(condition, cast.value, cast.target, instruction.range,
                      output);
  output << "  " << result_name(instruction) << " = select i1 " << condition
         << ", ptr " << value(cast.value) << ", ptr null\n";
}

void BodyEmitter::emit_type_condition(std::string_view result,
                                      MirValueId source, TypeId target,
                                      SourceRange range,
                                      std::ostringstream& output) {
  if (target.value >= module_.semantics().types().size()) {
    module_.report(range, "checked operation has an unknown target type");
    return;
  }
  const SemanticType& type = module_.semantics().type(target);
  if (type.kind == TypeKind::kObject) {
    output << "  " << result << " = icmp ne ptr " << value(source)
           << ", null\n";
    return;
  }

  const std::string raw = next_address();
  if (type.kind == TypeKind::kString) {
    output << "  " << raw << " = call i8 @cloth_rt_object_is_kind(ptr "
           << value(source) << ", i64 "
           << static_cast<std::uint64_t>(ClothHeapObjectKind::kString) << ")\n";
  } else if (type.kind == TypeKind::kFileClass && type.file) {
    output << "  " << raw << " = call i8 @cloth_rt_object_is_type(ptr "
           << value(source) << ", ptr "
           << module_.type_descriptor_global(*type.file) << ")\n";
  } else {
    module_.report(range, "unsupported checked target reached LLVM lowering");
    return;
  }
  output << "  " << result << " = icmp ne i8 " << raw << ", 0\n";
}

void BodyEmitter::emit_call(const MirInstruction& instruction,
                            const MirCallInstruction& call,
                            std::ostringstream& output) {
  const SemanticSymbol& symbol = module_.semantics().symbol(call.callable);
  if (symbol.intrinsic != IntrinsicKind::kNone) {
    const std::size_t expected_arguments =
        symbol.intrinsic == IntrinsicKind::kPrintNewline ? 0U : 1U;
    if (call.arguments.size() != expected_arguments || instruction.result) {
      module_.report(instruction.range,
                     "invalid core intrinsic reached LLVM lowering");
      return;
    }
    if (symbol.intrinsic == IntrinsicKind::kPrintNewline) {
      output << "  call void @cloth_rt_print_newline()\n";
      return;
    }
    const std::string argument = value(call.arguments[0]);
    switch (symbol.intrinsic) {
      case IntrinsicKind::kPrintString:
        output << "  call void @cloth_rt_print(ptr " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintBool: {
        const std::string extended = next_address();
        output << "  " << extended << " = zext i1 " << argument << " to i8\n"
               << "  call void @cloth_rt_print_bool(i8 " << extended << ")\n";
        break;
      }
      case IntrinsicKind::kPrintChar:
        output << "  call void @cloth_rt_print_char(i32 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintInt8:
        output << "  call void @cloth_rt_print_i8(i8 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintInt16:
        output << "  call void @cloth_rt_print_i16(i16 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintInt32:
        output << "  call void @cloth_rt_print_i32(i32 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintInt64:
        output << "  call void @cloth_rt_print_i64(i64 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintUint8:
        output << "  call void @cloth_rt_print_u8(i8 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintUint16:
        output << "  call void @cloth_rt_print_u16(i16 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintUint32:
        output << "  call void @cloth_rt_print_u32(i32 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintUint64:
        output << "  call void @cloth_rt_print_u64(i64 " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintFloat32:
        output << "  call void @cloth_rt_print_f32(float " << argument << ")\n";
        break;
      case IntrinsicKind::kPrintFloat64:
        output << "  call void @cloth_rt_print_f64(double " << argument
               << ")\n";
        break;
      case IntrinsicKind::kPrintObject:
        output << "  call void @cloth_rt_print_object(ptr " << argument
               << ")\n";
        break;
      case IntrinsicKind::kPrintNewline:
      case IntrinsicKind::kNone:
        module_.report(instruction.range,
                       "invalid core intrinsic reached LLVM lowering");
        break;
    }
    if (symbol.name == "println") {
      output << "  call void @cloth_rt_print_newline()\n";
    }
    return;
  }
  const AbiCallable* callable = module_.find_callable(call.callable);
  if (callable == nullptr) {
    module_.report(instruction.range, "call has no ABI declaration");
    return;
  }
  if (instruction.result) {
    output << "  " << result_name(instruction) << " = ";
  } else {
    output << "  ";
  }
  output << "call " << module_.llvm_type(callable->return_type) << " @"
         << callable->mangled_name << '(';
  bool needs_comma = false;
  const bool has_receiver =
      !callable->parameters.empty() &&
      callable->parameters.front().kind == AbiParameterKind::kReceiver;
  if (has_receiver) {
    std::string receiver = receiver_;
    if (call.kind == MirCallKind::kClassQualified) {
      receiver = "null";
    } else if (call.kind == MirCallKind::kInstance && call.receiver) {
      receiver = value(*call.receiver);
    }
    output << "ptr " << receiver;
    needs_comma = true;
  }
  for (std::size_t index = 0; index < call.arguments.size(); ++index) {
    if (needs_comma) {
      output << ", ";
    }
    const std::size_t abi_index = index + (has_receiver ? 1U : 0U);
    const TypeId type = callable->parameters.at(abi_index).type;
    output << module_.llvm_type(type) << ' ' << value(call.arguments[index]);
    needs_comma = true;
  }
  output << ")\n";
}

void BodyEmitter::emit_phi(const MirInstruction& instruction,
                           const MirPhiInstruction& phi,
                           std::ostringstream& output) {
  output << "  " << result_name(instruction) << " = phi "
         << module_.llvm_type(instruction.type) << ' ';
  for (std::size_t index = 0; index < phi.incoming.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << "[ " << value(phi.incoming[index].value) << ", %bb"
           << phi.incoming[index].predecessor.value << " ]";
  }
  output << '\n';
}

void BodyEmitter::emit_terminator(const MirTerminator& terminator,
                                  bool is_reachable,
                                  std::ostringstream& output) {
  if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
    output << "  br label %bb" << jump->target.value << '\n';
  } else if (const auto* branch =
                 std::get_if<MirBranchTerminator>(&terminator.data)) {
    output << "  br i1 " << value(branch->condition) << ", label %bb"
           << branch->then_block.value << ", label %bb"
           << branch->else_block.value << '\n';
  } else if (const auto* return_terminator =
                 std::get_if<MirReturnTerminator>(&terminator.data)) {
    if (is_reachable) {
      emit_gc_epilogue(output);
    }
    if (is_constructor_) {
      output << "  ret ptr %self\n";
    } else if (return_terminator->value) {
      output << "  ret " << module_.llvm_type(return_type_) << ' '
             << value(*return_terminator->value) << '\n';
    } else {
      output << "  ret void\n";
    }
  } else {
    output << "  unreachable\n";
  }
}

std::string BodyEmitter::value(MirValueId id) const {
  if (id.value >= values_.size() || values_[id.value].empty()) {
    return "undef";
  }
  return values_[id.value];
}

TypeId BodyEmitter::value_type(MirValueId id) const {
  if (id.value >= value_types_.size()) {
    return module_.semantics().error_type();
  }
  return value_types_[id.value];
}

std::string BodyEmitter::result_name(const MirInstruction& instruction) const {
  return "%v" + std::to_string(instruction.result->value);
}

std::string BodyEmitter::symbol_address(SymbolId symbol) const {
  const SemanticSymbol& semantic_symbol = module_.semantics().symbol(symbol);
  if (semantic_symbol.kind == SymbolKind::kField && semantic_symbol.is_static) {
    const AbiStaticField* field = module_.find_static_field(symbol);
    if (field != nullptr) {
      return "@" + field->mangled_name;
    }
  }
  return "%s" + std::to_string(symbol.value);
}

std::string BodyEmitter::argument_name(SymbolId symbol) const {
  return "%arg" + std::to_string(symbol.value);
}

std::string BodyEmitter::gc_value_address(MirValueId value) const {
  return "%gc.v" + std::to_string(value.value);
}

bool BodyEmitter::has_gc_symbol_root(SymbolId symbol) const noexcept {
  return std::find(gc_symbol_roots_.begin(), gc_symbol_roots_.end(), symbol) !=
         gc_symbol_roots_.end();
}

bool BodyEmitter::is_string_like(TypeId type) const noexcept {
  if (type.value >= module_.semantics().types().size()) {
    return false;
  }
  const SemanticType& semantic_type = module_.semantics().type(type);
  if (semantic_type.kind == TypeKind::kString) {
    return true;
  }
  return semantic_type.kind == TypeKind::kNullable &&
         semantic_type.element_type &&
         module_.semantics().type(*semantic_type.element_type).kind ==
             TypeKind::kString;
}

bool BodyEmitter::has_receiver_root() const noexcept {
  return !is_constructor_ && receiver_ != "undef";
}

std::size_t BodyEmitter::gc_root_count() const noexcept {
  return gc_symbol_roots_.size() + gc_value_root_count_ +
         (has_receiver_root() ? 1U : 0U) + (is_constructor_ ? 1U : 0U);
}

std::string BodyEmitter::next_address() {
  return "%addr" + std::to_string(address_count_++);
}

}  // namespace

std::optional<LlvmIrModule> emit_llvm_ir(const MirModule& mir,
                                         const AbiModule& abi,
                                         const SemanticModel& semantics,
                                         DiagnosticEngine& diagnostics,
                                         LlvmIrOptions options) {
  return ModuleEmitter{mir, abi, semantics, diagnostics, options}.emit();
}

}  // namespace cloth
