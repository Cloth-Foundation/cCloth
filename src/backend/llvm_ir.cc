// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/backend/llvm_ir.h"

#include "cloth/abi/abi.h"
#include "cloth/abi/aggregate_limits.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/identity/package_identity.h"
#include "cloth/lexer/literal.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir.h"
#include "cloth/runtime/runtime.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/scalar_constants.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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

template <typename Value>
std::optional<std::string> format_floating_literal(Value value) {
  static_assert(std::is_same_v<Value, float> || std::is_same_v<Value, double>);
  static_assert(std::numeric_limits<Value>::is_iec559);
  if (!std::isfinite(value)) return std::nullopt;
  using Bits = std::conditional_t<std::is_same_v<Value, float>, std::uint32_t,
                                  std::uint64_t>;
  // A decimal that round-trips through float need not be accepted by LLVM's
  // decimal IR parser. Preserve the already-resolved IEEE bits directly.
  return "bitcast (i" + std::to_string(sizeof(Bits) * 8) + " " +
         std::to_string(std::bit_cast<Bits>(value)) + " to " +
         (std::is_same_v<Value, float> ? "float)" : "double)");
}

std::optional<std::string> lower_scalar_literal(
    const MirLiteralInstruction& literal, const AbiTypeLayout& type,
    TypeKind semantic_kind) {
  switch (literal.kind) {
    case LiteralKind::kEnum: {
      std::uint32_t tag = 0;
      const auto [end, error] =
          std::from_chars(literal.lexeme.data(),
                          literal.lexeme.data() + literal.lexeme.size(), tag);
      if (semantic_kind != TypeKind::kEnum || error != std::errc{} ||
          end != literal.lexeme.data() + literal.lexeme.size())
        return std::nullopt;
      return std::to_string(tag);
    }
    case LiteralKind::kInteger: {
      std::string_view magnitude = literal.lexeme;
      const bool negative = magnitude.starts_with('-');
      if (negative) magnitude.remove_prefix(1);
      std::uint64_t parsed = 0;
      const char* const begin = magnitude.data();
      const char* const end = begin + magnitude.size();
      const auto result = std::from_chars(begin, end, parsed);
      const std::optional<NumericTypeProperties> properties =
          numeric_type_properties(semantic_kind);
      if (properties &&
          properties->category == NumericCategory::kFloatingPoint) {
        if (negative || result.ec != std::errc{} || result.ptr != end) {
          return std::nullopt;
        }
        return semantic_kind == TypeKind::kFloat32
                   ? format_floating_literal(static_cast<float>(parsed))
                   : format_floating_literal(static_cast<double>(parsed));
      }
      const std::optional<std::uint64_t> bits =
          integer_constant_bits(magnitude, negative, semantic_kind);
      if (type.bit_width == 0 || result.ec != std::errc{} ||
          result.ptr != end || !bits) {
        return std::nullopt;
      }
      return std::to_string(*bits);
    }
    case LiteralKind::kFloat: {
      const char* const begin = literal.lexeme.data();
      const char* const end = begin + literal.lexeme.size();
      const std::optional<NumericTypeProperties> properties =
          numeric_type_properties(semantic_kind);
      if (properties &&
          properties->category != NumericCategory::kFloatingPoint) {
        double parsed = 0.0;
        const auto parsed_result =
            std::from_chars(begin, end, parsed, std::chars_format::general);
        const double truncated = std::trunc(parsed);
        const long double upper = std::ldexp(1.0L, 64);
        if (parsed_result.ec != std::errc{} || parsed_result.ptr != end ||
            !std::isfinite(parsed) || truncated < 0.0 ||
            static_cast<long double>(truncated) >= upper) {
          return std::nullopt;
        }
        return std::to_string(static_cast<std::uint64_t>(truncated));
      }
      if (semantic_kind == TypeKind::kFloat32) {
        float parsed = 0.0F;
        const auto parsed_result =
            std::from_chars(begin, end, parsed, std::chars_format::general);
        if (parsed_result.ec != std::errc{} || parsed_result.ptr != end) {
          return std::nullopt;
        }
        return format_floating_literal(parsed);
      }
      double parsed = 0.0;
      const auto parsed_result =
          std::from_chars(begin, end, parsed, std::chars_format::general);
      if (semantic_kind != TypeKind::kFloat64 ||
          parsed_result.ec != std::errc{} || parsed_result.ptr != end) {
        return std::nullopt;
      }
      return format_floating_literal(parsed);
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

std::vector<MirValueId> instruction_value_uses(
    const MirInstruction& instruction) {
  std::vector<MirValueId> uses;
  const MirStoragePath* path = nullptr;
  if (const auto* load =
          std::get_if<MirLoadStorageInstruction>(&instruction.data))
    path = &load->path;
  if (const auto* store =
          std::get_if<MirStoreStorageInstruction>(&instruction.data)) {
    path = &store->path;
    uses.push_back(store->value);
  }
  if (path != nullptr) {
    if (path->object) uses.push_back(*path->object);
    if (path->index) uses.push_back(*path->index);
    return uses;
  }
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
  } else if (const auto* write =
                 std::get_if<MirIntegerWriteInstruction>(&instruction.data)) {
    uses.push_back(write->value);
    uses.push_back(write->destination);
    uses.push_back(write->offset);
  } else if (const auto* read =
                 std::get_if<MirIntegerReadInstruction>(&instruction.data)) {
    uses.push_back(read->source);
    uses.push_back(read->offset);
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

std::optional<SymbolId> storage_symbol_use(const MirInstruction& instruction) {
  if (const auto* load =
          std::get_if<MirLoadSymbolInstruction>(&instruction.data))
    return load->symbol;
  if (const auto* load =
          std::get_if<MirLoadStorageInstruction>(&instruction.data))
    return load->path.symbol;
  if (const auto* store =
          std::get_if<MirStoreStorageInstruction>(&instruction.data))
    return store->path.symbol;
  return std::nullopt;
}

std::vector<MirValueId> terminator_value_uses(const MirTerminator& terminator) {
  if (const auto* branch = std::get_if<MirBranchTerminator>(&terminator.data)) {
    return {branch->condition};
  }
  if (const auto* returned = std::get_if<MirReturnTerminator>(&terminator.data);
      returned != nullptr && returned->value) {
    return {*returned->value};
  }
  if (const auto* selection =
          std::get_if<MirSwitchTerminator>(&terminator.data))
    return {selection->selector};
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
              bool allocates_constructor, const AbiCallable* callable);

  [[nodiscard]] std::string emit();

 private:
  void prepare_values();
  void collect_storage();
  void collect_gc_roots();
  bool validate_aggregate_frame();
  bool has_struct_receiver() const;
  bool is_managed_constructor() const;
  bool is_aggregate_parameter(SymbolId symbol) const;
  void copy_value(TypeId type, std::string_view destination,
                  std::string_view source, std::ostringstream& output) const;
  void zero_root(TypeId type, std::string_view address,
                 std::ostringstream& output) const;
  void load_value(const MirInstruction& instruction, std::string_view address,
                  std::ostringstream& output);
  void store_value(TypeId type, std::string_view address, MirValueId value,
                   std::ostringstream& output);
  std::string storage_address(const MirStoragePath& path,
                              std::ostringstream& output);
  bool has_aggregate_phi(std::size_t block) const;
  bool needs_edge_block(std::size_t predecessor, std::size_t successor) const;
  std::string edge_label(std::size_t predecessor, std::size_t successor) const;
  void emit_phi_edges(std::ostringstream& output);
  std::string call_argument(const MirInstruction& instruction,
                            std::size_t index) const;
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
  void emit_integer_write(const MirInstruction& instruction,
                          const MirIntegerWriteInstruction& write,
                          std::ostringstream& output);
  void emit_integer_read(const MirInstruction& instruction,
                         const MirIntegerReadInstruction& read,
                         std::ostringstream& output);
  void emit_unary(const MirInstruction& instruction,
                  const MirUnaryInstruction& unary, std::ostringstream& output);
  void emit_integer_arithmetic_guard(std::string_view valid,
                                     std::uint8_t reason,
                                     std::ostringstream& output);
  void emit_binary(const MirInstruction& instruction,
                   const MirBinaryInstruction& binary,
                   std::ostringstream& output);
  void emit_conversion(const MirInstruction& instruction,
                       const MirConvertInstruction& conversion,
                       std::ostringstream& output);
  void emit_checked_numeric_conversion(const MirInstruction& instruction,
                                       const MirConvertInstruction& conversion,
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
  bool allocates_constructor_;
  const AbiCallable* callable_;
  std::vector<std::string> values_;
  std::vector<TypeId> value_types_;
  std::vector<SymbolId> storage_symbols_;
  std::vector<SymbolId> gc_symbol_roots_;
  std::vector<bool> gc_value_roots_;
  std::vector<std::vector<MirBlockId>> successors_;
  std::vector<std::vector<std::size_t>> predecessors_;
  std::vector<GcLiveSet> gc_live_in_;
  std::vector<GcLiveSet> gc_live_out_;
  std::vector<GcLiveSet> gc_live_after_phis_;
  std::vector<std::vector<GcLiveSet>> gc_live_after_instructions_;
  std::size_t gc_value_root_count_{0};
  std::size_t address_count_{0};
  std::size_t current_block_{0};
  struct ArgumentSlot {
    const MirInstruction* call;
    std::size_t index;
    TypeId type;
    std::string name;
  };
  std::vector<ArgumentSlot> argument_slots_;
  std::vector<MirValueId> aggregate_phis_;
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
    if (options_.package &&
        (options_.emit_native_entry_point || options_.entry_file ||
         !is_valid_package_name(options_.package->name) ||
         !is_valid_package_version(options_.package->version) ||
         std::ranges::none_of(
             semantics_.files(), [&](const FileSemantics& file) {
               return file.identity.package == *options_.package;
             }))) {
      report(fallback_range(), "invalid package ownership or entry options");
      return std::nullopt;
    }
    if (mir_.files.size() != abi_.files.size()) {
      report(fallback_range(), "MIR and ABI file counts differ");
      return std::nullopt;
    }
    type_descriptor_globals_.resize(mir_.files.size());
    for (std::size_t index = 0; index < type_descriptor_globals_.size();
         ++index) {
      type_descriptor_globals_[index] =
          type_descriptor_global_name(FileId{index});
    }
    for (const AbiFileClass& file : abi_.files) {
      if (file.kind == FileTypeKind::kClass) {
        if (owns(file)) {
          add_type_descriptor(file.file, *file.type_descriptor);
        } else {
          globals_.push_back(type_descriptor_global_name(file.file) +
                             " = external constant { i64, ptr, ptr, i64, i64, "
                             "i64, ptr, i64, ptr, i64, ptr, i64 }");
        }
      }
    }
    for (std::size_t index = 0; index < mir_.files.size(); ++index) {
      if (abi_.files[index].kind == FileTypeKind::kClass ||
          abi_.files[index].kind == FileTypeKind::kStruct) {
        if (owns(abi_.files[index])) {
          emit_file(mir_.files[index], abi_.files[index]);
        } else {
          emit_imported_declarations(abi_.files[index]);
        }
      }
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
           << "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)\n"
           << "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n"
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
           << "declare i8 @cloth_rt_object_is_interface(ptr, i64)\n"
           << "declare ptr @cloth_rt_interface_function(ptr, i64, i64)\n"
           << "declare ptr @cloth_rt_array_alloc(i32, ptr)\n"
           << "declare i32 @cloth_rt_array_length(ptr)\n"
           << "declare ptr @cloth_rt_array_element(ptr, i32)\n"
           << "declare void @cloth_rt_integer_write(ptr, i32, i64, i8, i8)\n"
           << "declare i64 @cloth_rt_integer_read(ptr, i32, i8, i8)\n"
           << "declare void @cloth_rt_require_receiver(ptr)\n"
           << "declare void @cloth_rt_require_non_null(ptr)\n"
           << "declare void @cloth_rt_require_numeric_conversion(i8)\n"
           << "declare void @cloth_rt_require_shift_count(i8)\n"
           << "declare void @cloth_rt_require_integer_arithmetic(i8, i8)\n"
           << "declare float @llvm.trunc.f32(float)\n"
           << "declare double @llvm.trunc.f64(double)\n"
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
    for (const unsigned int width : {8U, 16U, 32U, 64U}) {
      for (const std::string_view sign : {"s", "u"}) {
        for (const std::string_view operation : {"add", "sub", "mul"}) {
          output << "declare { i" << width << ", i1 } @llvm." << sign
                 << operation << ".with.overflow.i" << width << "(i" << width
                 << ", i" << width << ")\n";
        }
      }
    }
    output << '\n';
    output << "declare void @llvm.trap()\n\n";
    for (const std::string& global : globals_) {
      output << global << '\n';
    }
    if (!globals_.empty()) {
      output << '\n';
    }
    output << enum_helpers_.str() << aggregate_helpers_.str()
           << definitions_.str();
    return LlvmIrModule{output.str()};
  }

  std::string aggregate_comparer(TypeId type) {
    const auto name = "@.cloth.struct.equal." + std::to_string(type.value);
    if (std::ranges::find(aggregate_comparers_, type) !=
        aggregate_comparers_.end())
      return name;
    aggregate_comparers_.push_back(type);
    const auto& fields =
        abi_.files.at(semantics_.type(type).file->value).layout.fields;
    std::ostringstream body;
    body << "define internal i1 " << name
         << "(ptr %left, ptr %right) {\nentry:\n";
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto& field = fields[index];
      const auto suffix = std::to_string(index);
      body << "  %l" << suffix << " = getelementptr i8, ptr %left, i64 "
           << field.offset << '\n'
           << "  %r" << suffix << " = getelementptr i8, ptr %right, i64 "
           << field.offset << '\n';
      if (is_aggregate(field.type)) {
        body << "  %equal" << suffix << " = call i1 "
             << aggregate_comparer(field.type) << "(ptr %l" << suffix
             << ", ptr %r" << suffix << ")\n";
      } else {
        const auto llvm = llvm_type(field.type);
        body << "  %lv" << suffix << " = load " << llvm << ", ptr %l" << suffix
             << ", align " << alignment(field.type) << '\n'
             << "  %rv" << suffix << " = load " << llvm << ", ptr %r" << suffix
             << ", align " << alignment(field.type) << '\n';
        const auto& declared = semantics_.type(field.type);
        const auto kind =
            declared.kind == TypeKind::kNullable && declared.element_type
                ? semantics_.type(*declared.element_type).kind
                : declared.kind;
        if (kind == TypeKind::kString) {
          body << "  %raw" << suffix
               << " = call i8 @cloth_rt_string_equal(ptr %lv" << suffix
               << ", ptr %rv" << suffix << ")\n"
               << "  %equal" << suffix << " = icmp ne i8 %raw" << suffix
               << ", 0\n";
        } else {
          body << "  %equal" << suffix
               << (kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64
                       ? " = fcmp oeq "
                       : " = icmp eq ")
               << llvm << " %lv" << suffix << ", %rv" << suffix << '\n';
        }
      }
      body << "  br i1 %equal" << suffix << ", label %next" << suffix
           << ", label %unequal\nnext" << suffix << ":\n";
    }
    body << "  ret i1 true\n";
    if (!fields.empty()) body << "unequal:\n  ret i1 false\n";
    body << "}\n\n";
    aggregate_helpers_ << body.str();
    return name;
  }

  std::string enum_printer(TypeId type) {
    const std::string name = "@.cloth.enum.print." + std::to_string(type.value);
    if (std::ranges::find(enum_printers_, type) != enum_printers_.end())
      return name;
    enum_printers_.push_back(type);
    const SemanticType& enum_type = semantics_.type(type);
    const FileSemantics& file = semantics_.file(*enum_type.file);
    const std::string table =
        "@.cloth.enum.names." + std::to_string(type.value);
    const std::string table_type =
        "[" + std::to_string(file.enum_cases.size()) + " x { ptr, i64 }]";
    std::ostringstream entries;
    entries << table << " = private constant " << table_type << " [";
    for (std::size_t index = 0; index < file.enum_cases.size(); ++index) {
      if (index != 0) entries << ", ";
      const std::string display =
          enum_type.name + "." + semantics_.symbol(file.enum_cases[index]).name;
      const std::string bytes = add_string_literal(display);
      entries << "{ ptr, i64 } { ptr " << bytes << ", i64 " << display.size()
              << " }";
    }
    entries << "]";
    globals_.push_back(entries.str());
    enum_helpers_
        << "define internal void " << name << "(i32 %tag) {\nentry:\n"
        << "  %valid = icmp ult i32 %tag, " << file.enum_cases.size() << "\n"
        << "  br i1 %valid, label %read, label %invalid\ninvalid:\n"
        << "  call void @llvm.trap()\n  unreachable\nread:\n"
        << "  %index = zext i32 %tag to i64\n"
        << "  %record = getelementptr " << table_type << ", ptr " << table
        << ", i64 0, i64 %index\n"
        << "  %bytes = load ptr, ptr %record\n"
        << "  %lengthptr = getelementptr { ptr, i64 }, ptr %record, i32 0, i32 "
           "1\n"
        << "  %length = load i64, ptr %lengthptr\n"
        << "  %text = call ptr @cloth_rt_string_literal(ptr %bytes, i64 "
           "%length)\n"
        << "  call void @cloth_rt_print(ptr %text)\n  ret void\n}\n\n";
    return name;
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
      case AbiTypeKind::kAggregate:
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

  bool is_aggregate(TypeId type) const {
    return type.value < abi_.types.size() &&
           abi_.types[type.value].kind == AbiTypeKind::kAggregate;
  }

  bool has_references(TypeId type) const {
    return type.value < abi_.types.size() &&
           !abi_.types[type.value].reference_offsets.empty();
  }

  std::string storage_type(TypeId type) const {
    return is_aggregate(type)
               ? "[" + std::to_string(abi_.types.at(type.value).storage.size) +
                     " x i8]"
               : llvm_type(type);
  }

  std::string return_type(TypeId type) const {
    return is_aggregate(type) ? "void" : llvm_type(type);
  }

  std::string array_element_layout(TypeId type) {
    const std::string name =
        "@.cloth.array.element." + std::to_string(type.value);
    if (std::ranges::find(array_layouts_, type) != array_layouts_.end())
      return name;
    array_layouts_.push_back(type);
    const AbiTypeLayout& layout = abi_.types.at(type.value);
    const auto& offsets = layout.reference_offsets;
    std::string references = "null";
    if (!offsets.empty()) {
      references = name + ".refs";
      std::ostringstream map;
      map << references << " = private unnamed_addr constant ["
          << offsets.size() << " x i64] [";
      for (std::size_t index = 0; index < offsets.size(); ++index) {
        if (index != 0) map << ", ";
        map << "i64 " << offsets[index];
      }
      map << ']';
      globals_.push_back(map.str());
    }
    std::ostringstream metadata;
    metadata << name
             << " = private unnamed_addr constant { i64, i64, ptr, i64 } "
             << "{ i64 " << layout.storage.size << ", i64 "
             << layout.storage.alignment << ", ptr " << references << ", i64 "
             << offsets.size() << " }";
    globals_.push_back(metadata.str());
    return name;
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

    std::string virtuals_global = "null";
    if (!descriptor.virtual_functions.empty()) {
      virtuals_global = "@.cloth.type.virtuals." + std::to_string(file.value);
      std::ostringstream virtuals;
      virtuals << virtuals_global << " = private constant ["
               << descriptor.virtual_functions.size() << " x ptr] [";
      for (std::size_t index = 0; index < descriptor.virtual_functions.size();
           ++index) {
        if (index != 0) {
          virtuals << ", ";
        }
        const AbiCallable* callable =
            find_callable(descriptor.virtual_functions[index]);
        if (callable == nullptr) {
          report(semantics_.symbol(descriptor.virtual_functions[index]).range,
                 "virtual function has no ABI declaration");
          virtuals << "ptr null";
        } else {
          virtuals << "ptr @" << callable->mangled_name;
        }
      }
      virtuals << ']';
      globals_.push_back(virtuals.str());
    }

    std::string interfaces_global = "null";
    if (!descriptor.interfaces.empty()) {
      std::vector<std::string> function_globals;
      function_globals.reserve(descriptor.interfaces.size());
      for (std::size_t interface_index = 0;
           interface_index < descriptor.interfaces.size(); ++interface_index) {
        const AbiTypeDescriptor::InterfaceDispatch& interface =
            descriptor.interfaces[interface_index];
        std::string functions_global = "null";
        if (!interface.functions.empty()) {
          functions_global = "@.cloth.type.interface.functions." +
                             std::to_string(file.value) + "." +
                             std::to_string(interface_index);
          std::ostringstream functions;
          functions << functions_global << " = private constant ["
                    << interface.functions.size() << " x ptr] [";
          for (std::size_t function_index = 0;
               function_index < interface.functions.size(); ++function_index) {
            if (function_index != 0) {
              functions << ", ";
            }
            const AbiCallable* callable =
                find_callable(interface.functions[function_index]);
            if (callable == nullptr) {
              report(
                  semantics_.symbol(interface.functions[function_index]).range,
                  "interface implementation has no ABI declaration");
              functions << "ptr null";
            } else {
              functions << "ptr @" << callable->mangled_name;
            }
          }
          functions << ']';
          globals_.push_back(functions.str());
        }
        function_globals.push_back(std::move(functions_global));
      }

      interfaces_global =
          "@.cloth.type.interfaces." + std::to_string(file.value);
      std::ostringstream interfaces;
      interfaces << interfaces_global << " = private constant ["
                 << descriptor.interfaces.size() << " x { i64, ptr, i64 }] [";
      for (std::size_t index = 0; index < descriptor.interfaces.size();
           ++index) {
        if (index != 0) {
          interfaces << ", ";
        }
        interfaces << "{ i64, ptr, i64 } { i64 "
                   << descriptor.interfaces[index].interface_id << ", ptr "
                   << function_globals[index] << ", i64 "
                   << descriptor.interfaces[index].functions.size() << " }";
      }
      interfaces << ']';
      globals_.push_back(interfaces.str());
    }

    const std::string descriptor_global = type_descriptor_global_name(file);
    const std::string parent_global =
        descriptor.parent_file
            ? type_descriptor_global_name(*descriptor.parent_file)
            : "null";
    std::ostringstream global;
    global << descriptor_global
           << " = constant { i64, ptr, ptr, i64, i64, i64, ptr, i64, "
              "ptr, i64, ptr, i64 } { i64 "
           << static_cast<std::uint64_t>(descriptor.kind) << ", ptr "
           << parent_global << ", ptr " << name_global << ", i64 "
           << descriptor.name.size() << ", i64 " << descriptor.size << ", i64 "
           << descriptor.alignment << ", ptr " << references_global << ", i64 "
           << descriptor.reference_offsets.size() << ", ptr " << virtuals_global
           << ", i64 " << descriptor.virtual_functions.size() << ", ptr "
           << interfaces_global << ", i64 " << descriptor.interfaces.size()
           << " }";
    globals_.push_back(global.str());
    return descriptor_global;
  }

  [[nodiscard]] const std::string& type_descriptor_global(FileId file) const {
    return type_descriptor_globals_.at(file.value);
  }

  std::string type_descriptor_global_name(FileId file) const {
    const auto& descriptor = abi_.files.at(file.value).type_descriptor;
    return descriptor ? "@" + descriptor->mangled_name : std::string{};
  }

  void report(SourceRange range, std::string message) {
    diagnostics_.error(range, "LLVM lowering error: " + message);
    is_valid_ = false;
  }

 private:
  bool owns(const AbiFileClass& file) const {
    return !options_.package ||
           semantics_.file(file.file).identity.package == *options_.package;
  }

  void emit_imported_callable(const AbiCallable& callable) {
    if (callable.linkage != AbiLinkage::kExternal) {
      return;
    }
    definitions_ << "declare " << return_type(callable.return_type) << " @"
                 << callable.mangled_name << '(';
    for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
      if (index != 0) {
        definitions_ << ", ";
      }
      definitions_ << llvm_type(callable.parameters[index].type);
    }
    definitions_ << ")\n";
    if (callable.kind == AbiCallableKind::kConstructor &&
        callable.initializer_linkage == AbiLinkage::kExternal) {
      definitions_ << "declare void @" << callable.initializer_mangled_name
                   << "(ptr";
      for (const AbiParameter& parameter : callable.parameters) {
        definitions_ << ", " << llvm_type(parameter.type);
      }
      definitions_ << ")\n";
    }
  }

  void emit_imported_declarations(const AbiFileClass& file) {
    for (const AbiStaticField& field : file.static_fields) {
      if (field.linkage == AbiLinkage::kExternal) {
        globals_.push_back("@" + field.mangled_name + " = external constant " +
                           llvm_type(field.type));
      }
    }
    for (const AbiCallable& callable : file.functions) {
      emit_imported_callable(callable);
    }
    for (const AbiCallable& callable : file.constructors) {
      emit_imported_callable(callable);
    }
  }

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
    if (abi_field == nullptr || !field.static_constant ||
        !is_valid_scalar_constant(*field.static_constant, abi_field->type,
                                  semantics_)) {
      report(semantics_.symbol(field.symbol).range,
             "static field has no verified scalar constant or ABI declaration");
      return;
    }
    const auto constant = *field.static_constant;
    const auto kind = semantics_.type(constant.type).kind;
    std::string value;
    if (kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64) {
      // LLVM accepts an exact integer-to-float constant bitcast. This preserves
      // signed zero and subnormals without host arithmetic or decimal
      // reparsing.
      value = "bitcast (i" +
              std::string{kind == TypeKind::kFloat32 ? "32" : "64"} + " " +
              std::to_string(constant.bits) + " to " +
              llvm_type(constant.type) + ")";
    } else {
      value = std::to_string(constant.bits);
    }
    std::ostringstream global;
    global << '@' << abi_field->mangled_name << " = ";
    if (abi_field->linkage == AbiLinkage::kInternal) {
      global << "internal ";
    }
    global << "constant " << llvm_type(abi_field->type) << ' ' << value
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
    definitions_ << "define internal " << return_type(field->type) << " @"
                 << name
                 << (is_aggregate(field->type)
                         ? "(ptr %result, ptr %receiver) {\n"
                         : "(ptr %receiver) {\n")
                 << BodyEmitter{*this,       body,  file,  field->type,
                                "%receiver", false, false, nullptr}
                        .emit()
                 << "}\n\n";
  }

  void emit_constructor_initializer(const AbiFileClass& file,
                                    const MirCallable& mir_callable,
                                    const AbiCallable& callable) {
    definitions_ << "define "
                 << (callable.initializer_linkage == AbiLinkage::kInternal
                         ? "internal "
                         : "")
                 << "void @" << callable.initializer_mangled_name
                 << "(ptr %self";
    for (const AbiParameter& parameter : callable.parameters) {
      definitions_ << ", " << llvm_type(parameter.type) << " %arg"
                   << parameter.symbol->value;
    }
    definitions_ << ") {\n"
                 << BodyEmitter{*this,   mir_callable.body,
                                file,    semantics_.void_type(),
                                "%self", true,
                                false,   &callable}
                        .emit()
                 << "}\n\n";
  }

  void emit_callable(const AbiFileClass& file, const MirCallable& mir_callable,
                     const AbiCallable& callable) {
    if (!callable.initializer_mangled_name.empty()) {
      emit_constructor_initializer(file, mir_callable, callable);
    }
    if (callable.linkage == AbiLinkage::kInternal) {
      definitions_ << "define internal ";
    } else {
      definitions_ << "define ";
    }
    definitions_ << return_type(callable.return_type) << " @"
                 << callable.mangled_name << '(';
    for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
      if (index != 0) {
        definitions_ << ", ";
      }
      const AbiParameter& parameter = callable.parameters[index];
      definitions_ << llvm_type(parameter.type) << ' ';
      if (parameter.kind == AbiParameterKind::kResult) {
        definitions_ << "%result";
      } else if (parameter.kind == AbiParameterKind::kReceiver) {
        definitions_ << "%receiver";
      } else {
        definitions_ << "%arg" << parameter.symbol->value;
      }
    }
    definitions_ << ") {\n";
    const bool is_constructor = callable.kind == AbiCallableKind::kConstructor;
    const bool has_receiver =
        callable.receiver_mode == AbiReceiverMode::kReference ||
        callable.receiver_mode == AbiReceiverMode::kReadOnlyValue;
    definitions_ << BodyEmitter{*this,
                                mir_callable.body,
                                file,
                                callable.return_type,
                                is_constructor
                                    ? (file.kind == FileTypeKind::kStruct
                                           ? "%result"
                                           : "%self")
                                    : (has_receiver ? "%receiver" : "undef"),
                                is_constructor,
                                is_constructor &&
                                    file.kind != FileTypeKind::kStruct,
                                &callable}
                        .emit()
                 << "}\n\n";
  }

  void emit_native_entry_point() {
    const AbiCallable* entry = find_native_entry_point(
        abi_, semantics_, diagnostics_, options_.entry_file);
    if (entry == nullptr) {
      is_valid_ = false;
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
  std::ostringstream enum_helpers_;
  std::ostringstream aggregate_helpers_;
  std::vector<TypeId> aggregate_comparers_;
  std::vector<TypeId> enum_printers_;
  std::vector<TypeId> array_layouts_;
  bool is_valid_{true};
};

BodyEmitter::BodyEmitter(ModuleEmitter& module, const MirBody& body,
                         const AbiFileClass& file, TypeId return_type,
                         std::string receiver, bool is_constructor,
                         bool allocates_constructor,
                         const AbiCallable* callable)
    : module_(module),
      body_(body),
      file_(file),
      return_type_(return_type),
      receiver_(std::move(receiver)),
      is_constructor_(is_constructor),
      allocates_constructor_(allocates_constructor),
      callable_(callable),
      values_(body.value_count),
      value_types_(body.value_count, module.semantics().error_type()) {
  successors_.reserve(body.blocks.size());
  predecessors_.resize(body.blocks.size());
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    successors_.push_back(mir_successors(body.blocks[index].terminator));
    for (const auto successor : successors_.back())
      predecessors_.at(successor.value).push_back(index);
  }
  prepare_values();
  collect_storage();
  collect_gc_roots();
  analyze_gc_liveness();
}

std::string BodyEmitter::emit() {
  if (!validate_aggregate_frame()) return {};
  std::ostringstream output;
  for (std::size_t block_index = 0; block_index < body_.blocks.size();
       ++block_index) {
    current_block_ = block_index;
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
  emit_phi_edges(output);
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
        if (module_.is_aggregate(instruction.type) &&
            std::holds_alternative<MirPhiInstruction>(instruction.data))
          aggregate_phis_.push_back(*instruction.result);
      }
    }
  }
}

void BodyEmitter::collect_storage() {
  for (const auto& block : body_.blocks) {
    for (const auto& instruction : block.instructions) {
      const auto* call = std::get_if<MirCallInstruction>(&instruction.data);
      if (!call || module_.semantics().symbol(call->callable).intrinsic !=
                       IntrinsicKind::kNone)
        continue;
      for (std::size_t index = 0; index < call->arguments.size(); ++index) {
        const TypeId type = value_type(call->arguments[index]);
        if (module_.is_aggregate(type))
          argument_slots_.push_back(
              {&instruction, index, type,
               "%call.arg." + std::to_string(argument_slots_.size())});
      }
    }
  }
  if (callable_ != nullptr) {
    for (const AbiParameter& parameter : callable_->parameters) {
      if (parameter.kind == AbiParameterKind::kExplicit) {
        storage_symbols_.push_back(*parameter.symbol);
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
    if (module_.has_references(semantics.symbol(symbol).type)) {
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
          module_.has_references(instruction.type)) {
        gc_value_roots_[instruction.result->value] = true;
        gc_value_root_count_ += module_.abi()
                                    .types.at(instruction.type.value)
                                    .reference_offsets.size();
      }
    }
  }
}

void BodyEmitter::analyze_gc_liveness() {
  // Receivers have independent roots. Bodies with no managed temporaries or
  // locals need no liveness tables, even for a full-width dense enum switch.
  if (gc_value_root_count_ == 0 && gc_symbol_roots_.empty()) return;
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
      if (const auto symbol = storage_symbol_use(instruction);
          symbol && has_gc_symbol_root(*symbol) &&
          !block_definitions[block_index].symbols[symbol->value]) {
        block_uses[block_index].symbols[symbol->value] = true;
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
      for (const MirBlockId successor : successors_[block_index]) {
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
      if (const auto symbol = storage_symbol_use(instruction))
        add_symbol(live, *symbol);
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

bool BodyEmitter::has_struct_receiver() const {
  return file_.kind == FileTypeKind::kStruct && receiver_ != "undef";
}

bool BodyEmitter::is_managed_constructor() const {
  return is_constructor_ && file_.kind == FileTypeKind::kClass;
}

bool BodyEmitter::is_aggregate_parameter(SymbolId symbol) const {
  return callable_ != nullptr &&
         std::ranges::any_of(
             callable_->parameters, [&](const AbiParameter& parameter) {
               return parameter.kind == AbiParameterKind::kExplicit &&
                      parameter.symbol == symbol &&
                      module_.is_aggregate(parameter.type);
             });
}

bool BodyEmitter::validate_aggregate_frame() {
  std::uint64_t total = 0;
  const auto add = [&](std::uint64_t bytes) {
    if (bytes > kMaxAggregateFrameSize - total) return false;
    total += bytes;
    return true;
  };
  const auto storage = [&](TypeId type) {
    return !module_.is_aggregate(type) ||
           add(module_.abi().types.at(type.value).storage.size);
  };
  bool valid = true;
  for (const TypeId type : value_types_) valid = valid && storage(type);
  for (const SymbolId symbol : storage_symbols_)
    if (!is_aggregate_parameter(symbol))
      valid = valid && storage(module_.semantics().symbol(symbol).type);
  for (const auto& slot : argument_slots_) valid = valid && storage(slot.type);
  for (const MirValueId id : aggregate_phis_)
    valid = valid && storage(value_type(id));
  // All aggregate root addresses count, including externally supplied
  // parameters.
  const auto count = [&](TypeId type) {
    if (!valid || !module_.is_aggregate(type)) return;
    const auto references =
        module_.abi().types.at(type.value).reference_offsets.size();
    const auto pointer_size = module_.abi().target.pointer.size;
    valid = references <= kMaxAggregateFrameSize / pointer_size &&
            add(references * pointer_size);
  };
  for (std::size_t index = 0; index < gc_value_roots_.size(); ++index)
    if (gc_value_roots_[index]) count(value_type(MirValueId{index}));
  for (const SymbolId symbol : gc_symbol_roots_)
    count(module_.semantics().symbol(symbol).type);
  for (const auto& slot : argument_slots_) count(slot.type);
  if (has_struct_receiver()) count(module_.semantics().file(file_.file).type);
  if (!valid)
    module_.report(body_.range,
                   "aggregate frame exceeds the 262144-byte limit");
  return valid;
}

void BodyEmitter::copy_value(TypeId type, std::string_view destination,
                             std::string_view source,
                             std::ostringstream& output) const {
  output << "  call void @llvm.memmove.p0.p0.i64(ptr align "
         << module_.alignment(type) << ' ' << destination << ", ptr align "
         << module_.alignment(type) << ' ' << source << ", i64 "
         << module_.abi().types.at(type.value).storage.size << ", i1 false)\n";
}

void BodyEmitter::zero_root(TypeId type, std::string_view address,
                            std::ostringstream& output) const {
  if (module_.is_aggregate(type)) {
    output << "  call void @llvm.memset.p0.i64(ptr align "
           << module_.alignment(type) << ' ' << address << ", i8 0, i64 "
           << module_.abi().types.at(type.value).storage.size
           << ", i1 false)\n";
  } else {
    output << "  store ptr null, ptr " << address << ", align "
           << module_.pointer_alignment() << '\n';
  }
}

void BodyEmitter::load_value(const MirInstruction& instruction,
                             std::string_view address,
                             std::ostringstream& output) {
  if (module_.is_aggregate(instruction.type)) {
    copy_value(instruction.type, result_name(instruction), address, output);
  } else {
    output << "  " << result_name(instruction) << " = load "
           << module_.llvm_type(instruction.type) << ", ptr " << address
           << ", align " << module_.alignment(instruction.type) << '\n';
  }
}

void BodyEmitter::store_value(TypeId type, std::string_view address,
                              MirValueId stored, std::ostringstream& output) {
  if (module_.is_aggregate(type))
    copy_value(type, address, value(stored), output);
  else
    output << "  store " << module_.llvm_type(type) << ' ' << value(stored)
           << ", ptr " << address << ", align " << module_.alignment(type)
           << '\n';
}

std::string BodyEmitter::storage_address(const MirStoragePath& path,
                                         std::ostringstream& output) {
  std::string address;
  if (path.symbol) {
    address = symbol_address(*path.symbol);
  } else if (path.index) {
    address = next_address();
    output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
           << value(*path.object) << ", i32 " << value(*path.index) << ")\n";
  } else {
    address = value(*path.object);
    output << "  call void @cloth_rt_require_receiver(ptr " << address << ")\n";
  }
  for (const SymbolId field_id : path.fields) {
    const auto* field = module_.find_field(field_id);
    if (!field) {
      module_.report(body_.range, "storage projection has no ABI field");
      return "undef";
    }
    const auto next = next_address();
    output << "  " << next << " = getelementptr i8, ptr " << address << ", i64 "
           << field->offset << '\n';
    address = next;
  }
  return address;
}

std::string BodyEmitter::call_argument(const MirInstruction& instruction,
                                       std::size_t index) const {
  for (const auto& slot : argument_slots_)
    if (slot.call == &instruction && slot.index == index) return slot.name;
  return {};
}

void BodyEmitter::emit_prologue(std::ostringstream& output) {
  const auto& semantics = module_.semantics();
  const auto pointer_alignment = module_.pointer_alignment();
  const auto allocate = [&](TypeId type, const std::string& address) {
    output << "  " << address << " = alloca " << module_.storage_type(type)
           << ", align " << module_.alignment(type) << '\n';
    if (module_.is_aggregate(type)) zero_root(type, address, output);
  };
  for (const SymbolId symbol : storage_symbols_) {
    if (!is_aggregate_parameter(symbol))
      allocate(semantics.symbol(symbol).type, symbol_address(symbol));
  }
  for (std::size_t index = 0; index < value_types_.size(); ++index) {
    const MirValueId id{index};
    if (module_.is_aggregate(value_type(id))) {
      allocate(value_type(id), value(id));
    } else if (gc_value_roots_[index]) {
      output << "  " << gc_value_address(id) << " = alloca ptr, align "
             << pointer_alignment << '\n';
      zero_root(value_type(id), gc_value_address(id), output);
    }
  }
  for (const auto& slot : argument_slots_) allocate(slot.type, slot.name);
  for (const MirValueId id : aggregate_phis_)
    allocate(value_type(id), "%phi.copy." + std::to_string(id.value));
  if (has_receiver_root())
    output << "  %gc.receiver = alloca ptr, align " << pointer_alignment
           << '\n';
  if (is_managed_constructor())
    output << "  %gc.self = alloca ptr, align " << pointer_alignment << '\n';

  for (const SymbolId symbol : gc_symbol_roots_) {
    if (!module_.is_aggregate(semantics.symbol(symbol).type))
      zero_root(semantics.symbol(symbol).type, symbol_address(symbol), output);
  }
  if (callable_ != nullptr) {
    for (const auto& parameter : callable_->parameters) {
      if (parameter.kind == AbiParameterKind::kExplicit &&
          !module_.is_aggregate(parameter.type)) {
        output << "  store " << module_.llvm_type(parameter.type) << ' '
               << argument_name(*parameter.symbol) << ", ptr "
               << symbol_address(*parameter.symbol) << ", align "
               << module_.alignment(parameter.type) << '\n';
      }
    }
  }
  if (has_receiver_root())
    output << "  store ptr " << receiver_ << ", ptr %gc.receiver, align "
           << pointer_alignment << '\n';
  if (is_managed_constructor())
    output << "  store ptr " << (allocates_constructor_ ? "null" : receiver_)
           << ", ptr %gc.self, align " << pointer_alignment << '\n';

  const auto root_count = gc_root_count();
  if (root_count != 0) {
    output << "  %gc.frame = alloca { ptr, ptr, i64 }, align "
           << module_.gc_frame_alignment() << '\n'
           << "  %gc.roots = alloca [" << root_count << " x ptr], align "
           << pointer_alignment << '\n';
    std::vector<std::string> roots;
    roots.reserve(root_count);
    const auto append = [&](TypeId type, const std::string& address) {
      for (const auto offset :
           module_.abi().types.at(type.value).reference_offsets) {
        const std::string slot = next_address();
        output << "  " << slot << " = getelementptr i8, ptr " << address
               << ", i64 " << offset << '\n';
        roots.push_back(slot);
      }
    };
    if (has_receiver_root()) roots.emplace_back("%gc.receiver");
    if (is_managed_constructor()) roots.emplace_back("%gc.self");
    if (has_struct_receiver())
      append(semantics.file(file_.file).type, receiver_);
    for (const SymbolId symbol : gc_symbol_roots_) {
      if (module_.is_aggregate(semantics.symbol(symbol).type))
        append(semantics.symbol(symbol).type, symbol_address(symbol));
      else
        roots.push_back(symbol_address(symbol));
    }
    for (std::size_t index = 0; index < gc_value_roots_.size(); ++index) {
      if (!gc_value_roots_[index]) continue;
      const MirValueId id{index};
      if (module_.is_aggregate(value_type(id)))
        append(value_type(id), value(id));
      else
        roots.push_back(gc_value_address(id));
    }
    for (const auto& slot : argument_slots_) append(slot.type, slot.name);
    for (std::size_t index = 0; index < roots.size(); ++index) {
      output << "  %gc.root." << index << " = getelementptr [" << root_count
             << " x ptr], ptr %gc.roots, i64 0, i64 " << index << '\n'
             << "  store ptr " << roots[index] << ", ptr %gc.root." << index
             << ", align " << pointer_alignment << '\n';
    }
    output << "  call void @cloth_rt_gc_push_frame(ptr %gc.frame, ptr "
              "%gc.roots, i64 "
           << root_count << ")\n";
  }
  if (allocates_constructor_) {
    output << "  %self = call ptr @cloth_rt_alloc(ptr "
           << module_.type_descriptor_global(file_.file) << ")\n"
           << "  store ptr %self, ptr %gc.self, align " << pointer_alignment
           << '\n';
  }
}

void BodyEmitter::emit_gc_value_root(const MirInstruction& instruction,
                                     std::ostringstream& output) const {
  if (!instruction.result ||
      instruction.result->value >= gc_value_roots_.size() ||
      !gc_value_roots_[instruction.result->value] ||
      module_.is_aggregate(instruction.type)) {
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
          has_gc_symbol_root(*parameter.symbol)) {
        candidates.symbols[parameter.symbol->value] = true;
      }
    }
  }
  for (const std::size_t predecessor : predecessors_[block]) {
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
  for (const SymbolId symbol : gc_symbol_roots_) {
    if (candidates.symbols[symbol.value] && !live.symbols[symbol.value]) {
      zero_root(module_.semantics().symbol(symbol).type, symbol_address(symbol),
                output);
    }
  }
  for (std::size_t index = 0; index < gc_value_roots_.size(); ++index) {
    if (candidates.values[index] && !live.values[index]) {
      zero_root(value_type(MirValueId{index}),
                gc_value_address(MirValueId{index}), output);
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

  for (std::size_t index = 0; index < clear_values.size(); ++index) {
    if (clear_values[index]) {
      zero_root(value_type(MirValueId{index}),
                gc_value_address(MirValueId{index}), output);
    }
  }

  std::optional<SymbolId> touched_symbol = storage_symbol_use(instruction);
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
    zero_root(module_.semantics().symbol(*touched_symbol).type,
              symbol_address(*touched_symbol), output);
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
    output << "  " << address << " = getelementptr i8, ptr " << receiver_
           << ", i64 " << field.offset << '\n';
    if (module_.is_aggregate(field.type)) {
      // This internal helper initializes an unpublished field. Its containing
      // object/result is already rooted; no source-visible value aliases it.
      output << "  call void @" << initializer << "(ptr " << address << ", ptr "
             << receiver_ << ")\n";
    } else {
      output << "  " << value_name << " = call "
             << module_.llvm_type(field.type) << " @" << initializer << "(ptr "
             << receiver_ << ")\n"
             << "  store " << module_.llvm_type(field.type) << ' ' << value_name
             << ", ptr " << address << ", align "
             << module_.alignment(field.type) << '\n';
    }
  }
}

void BodyEmitter::emit_instruction(const MirInstruction& instruction,
                                   std::ostringstream& output) {
  if (const auto* load =
          std::get_if<MirLoadStorageInstruction>(&instruction.data)) {
    load_value(instruction, storage_address(load->path, output), output);
  } else if (const auto* store =
                 std::get_if<MirStoreStorageInstruction>(&instruction.data)) {
    store_value(value_type(store->value), storage_address(store->path, output),
                store->value, output);
  } else if (const auto* literal =
                 std::get_if<MirLiteralInstruction>(&instruction.data)) {
    emit_literal(instruction, *literal, output);
  } else if (const auto* load =
                 std::get_if<MirLoadSymbolInstruction>(&instruction.data)) {
    emit_load_symbol(instruction, *load, output);
  } else if (const auto* declaration =
                 std::get_if<MirDeclareLocalInstruction>(&instruction.data)) {
    if (declaration->initializer) {
      const TypeId type = module_.semantics().symbol(declaration->symbol).type;
      store_value(type, symbol_address(declaration->symbol),
                  *declaration->initializer, output);
    }
  } else if (const auto* store =
                 std::get_if<MirStoreSymbolInstruction>(&instruction.data)) {
    const TypeId type = module_.semantics().symbol(store->symbol).type;
    store_value(type, symbol_address(store->symbol), store->value, output);
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
  } else if (const auto* write =
                 std::get_if<MirIntegerWriteInstruction>(&instruction.data)) {
    emit_integer_write(instruction, *write, output);
  } else if (const auto* read =
                 std::get_if<MirIntegerReadInstruction>(&instruction.data)) {
    emit_integer_read(instruction, *read, output);
  } else if (const auto* unary =
                 std::get_if<MirUnaryInstruction>(&instruction.data)) {
    emit_unary(instruction, *unary, output);
  } else if (const auto* binary =
                 std::get_if<MirBinaryInstruction>(&instruction.data)) {
    emit_binary(instruction, *binary, output);
  } else if (const auto* conversion =
                 std::get_if<MirConvertInstruction>(&instruction.data)) {
    emit_conversion(instruction, *conversion, output);
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
  } else if (std::holds_alternative<MirInitializeFieldsInstruction>(
                 instruction.data)) {
    emit_field_initializers(output);
  } else if (std::holds_alternative<MirInvalidInstruction>(instruction.data)) {
    module_.report(instruction.range, "invalid MIR reached LLVM lowering");
  }
}

void BodyEmitter::emit_literal(const MirInstruction& instruction,
                               const MirLiteralInstruction& literal,
                               std::ostringstream& output) {
  std::string lowered;
  switch (literal.kind) {
    case LiteralKind::kEnum:
      if (!enum_constant_tag(literal.lexeme, instruction.type,
                             module_.semantics())) {
        module_.report(instruction.range,
                       "invalid enum constant reached LLVM lowering");
        return;
      }
      lowered = literal.lexeme;
      break;
    case LiteralKind::kInteger:
    case LiteralKind::kFloat:
    case LiteralKind::kCharacter:
    case LiteralKind::kBoolean: {
      const std::optional<std::string> value = lower_scalar_literal(
          literal, module_.abi().types.at(instruction.type.value),
          module_.semantics().type(instruction.type).kind);
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
    if (module_.is_aggregate(instruction.type))
      copy_value(instruction.type, result_name(instruction), receiver_, output);
    else
      values_.at(instruction.result->value) = receiver_;
    return;
  }
  load_value(instruction, symbol_address(load.symbol), output);
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
  if (!module_.is_aggregate(value_type(load.object)))
    output << "  call void @cloth_rt_require_receiver(ptr " << object << ")\n";
  output << "  " << address << " = getelementptr i8, ptr " << object << ", i64 "
         << field->offset << '\n';
  load_value(instruction, address, output);
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
         << field->offset << '\n';
  store_value(field->type, address, store.value, output);
}

void BodyEmitter::emit_array_literal(const MirInstruction& instruction,
                                     const MirArrayLiteralInstruction& array,
                                     std::ostringstream& output) {
  if (array.elements.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    module_.report(instruction.range, "array literal has too many elements");
    return;
  }
  output << "  " << result_name(instruction)
         << " = call ptr @cloth_rt_array_alloc(i32 " << array.elements.size()
         << ", ptr " << module_.array_element_layout(array.element_type)
         << ")\n";
  for (std::size_t index = 0; index < array.elements.size(); ++index) {
    const std::string address = next_address();
    output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
           << result_name(instruction) << ", i32 " << index << ")\n";
    store_value(array.element_type, address, array.elements[index], output);
  }
}

void BodyEmitter::emit_array_load(const MirInstruction& instruction,
                                  const MirArrayLoadInstruction& load,
                                  std::ostringstream& output) {
  const std::string address = next_address();
  output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
         << value(load.array) << ", i32 " << value(load.index) << ")\n";
  load_value(instruction, address, output);
}

void BodyEmitter::emit_array_store(const MirInstruction&,
                                   const MirArrayStoreInstruction& store,
                                   std::ostringstream& output) {
  const TypeId type = value_type(store.value);
  const std::string address = next_address();
  output << "  " << address << " = call ptr @cloth_rt_array_element(ptr "
         << value(store.array) << ", i32 " << value(store.index) << ")\n";
  store_value(type, address, store.value, output);
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
  const SemanticType& type = module_.semantics().type(value_type(meta.object));
  if (type.kind == TypeKind::kEnum || type.kind == TypeKind::kStruct) {
    const std::string bytes = module_.add_string_literal(type.name);
    output << "  " << result_name(instruction)
           << " = call ptr @cloth_rt_string_literal(ptr " << bytes << ", i64 "
           << type.name.size() << ")\n";
    return;
  }
  output << "  " << result_name(instruction)
         << " = call ptr @cloth_rt_object_type_name(ptr " << value(meta.object)
         << ")\n";
}

void BodyEmitter::emit_integer_write(const MirInstruction& instruction,
                                     const MirIntegerWriteInstruction& write,
                                     std::ostringstream& output) {
  const TypeId integer_type = value_type(write.value);
  const std::optional<NumericTypeProperties> properties =
      numeric_type_properties(module_.semantics().type(integer_type).kind);
  if (!properties || properties->category == NumericCategory::kFloatingPoint) {
    module_.report(instruction.range,
                   "non-integer endian write reached LLVM lowering");
    return;
  }
  std::string bits = value(write.value);
  if (properties->bit_width < 64) {
    const std::string widened = next_address();
    output << "  " << widened << " = zext " << module_.llvm_type(integer_type)
           << ' ' << bits << " to i64\n";
    bits = widened;
  }
  output << "  call void @cloth_rt_integer_write(ptr "
         << value(write.destination) << ", i32 " << value(write.offset)
         << ", i64 " << bits << ", i8 " << properties->bit_width / 8 << ", i8 "
         << (write.byte_order == IntegerByteOrder::kLittleEndian ? 0 : 1)
         << ")\n";
}

void BodyEmitter::emit_integer_read(const MirInstruction& instruction,
                                    const MirIntegerReadInstruction& read,
                                    std::ostringstream& output) {
  const std::optional<NumericTypeProperties> properties =
      numeric_type_properties(module_.semantics().type(instruction.type).kind);
  if (!properties || properties->category == NumericCategory::kFloatingPoint) {
    module_.report(instruction.range,
                   "non-integer endian read reached LLVM lowering");
    return;
  }
  if (properties->bit_width == 64) {
    output << "  " << result_name(instruction)
           << " = call i64 @cloth_rt_integer_read(ptr " << value(read.source)
           << ", i32 " << value(read.offset) << ", i8 8, i8 "
           << (read.byte_order == IntegerByteOrder::kLittleEndian ? 0 : 1)
           << ")\n";
    return;
  }
  const std::string bits = next_address();
  output << "  " << bits << " = call i64 @cloth_rt_integer_read(ptr "
         << value(read.source) << ", i32 " << value(read.offset) << ", i8 "
         << properties->bit_width / 8 << ", i8 "
         << (read.byte_order == IntegerByteOrder::kLittleEndian ? 0 : 1)
         << ")\n"
         << "  " << result_name(instruction) << " = trunc i64 " << bits
         << " to " << module_.llvm_type(instruction.type) << '\n';
}

void BodyEmitter::emit_integer_arithmetic_guard(std::string_view valid,
                                                std::uint8_t reason,
                                                std::ostringstream& output) {
  const std::string valid_byte = next_address();
  output << "  " << valid_byte << " = zext i1 " << valid << " to i8\n"
         << "  call void @cloth_rt_require_integer_arithmetic(i8 " << valid_byte
         << ", i8 " << static_cast<unsigned int>(reason) << ")\n";
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
  const auto properties = numeric_type_properties(kind);
  if (unary.operation == TokenKind::kMinus && properties &&
      properties->category != NumericCategory::kFloatingPoint) {
    const bool is_unsigned =
        properties->category == NumericCategory::kUnsignedInteger;
    const std::string aggregate = next_address();
    const std::string overflow = next_address();
    const std::string valid = next_address();
    output << "  " << aggregate << " = call { " << type << ", i1 } @llvm."
           << (is_unsigned ? 'u' : 's') << "sub.with.overflow.i"
           << properties->bit_width << '(' << type << " 0, " << type << ' '
           << operand << ")\n"
           << "  " << result_name(instruction) << " = extractvalue { " << type
           << ", i1 } " << aggregate << ", 0\n"
           << "  " << overflow << " = extractvalue { " << type << ", i1 } "
           << aggregate << ", 1\n"
           << "  " << valid << " = xor i1 " << overflow << ", true\n";
    emit_integer_arithmetic_guard(valid, kClothIntegerArithmeticOverflow,
                                  output);
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
  if (kind == TypeKind::kStruct) {
    const std::string equal = next_address();
    output << "  " << equal << " = call i1 "
           << module_.aggregate_comparer(operand_type) << "(ptr "
           << value(binary.left) << ", ptr " << value(binary.right) << ")\n"
           << "  " << result_name(instruction) << " = xor i1 " << equal << ", "
           << (binary.operation == TokenKind::kBangEqual ? "true" : "false")
           << '\n';
    return;
  }
  const bool is_float =
      kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64;
  const bool is_unsigned =
      kind == TypeKind::kByte || kind == TypeKind::kChar ||
      kind == TypeKind::kUint8 || kind == TypeKind::kUint16 ||
      kind == TypeKind::kUint32 || kind == TypeKind::kUint64;
  const std::optional<NumericTypeProperties> integer_properties =
      numeric_type_properties(kind);
  const bool is_integer =
      integer_properties &&
      integer_properties->category != NumericCategory::kFloatingPoint;
  const std::string type = module_.llvm_type(operand_type);
  if (is_integer && (binary.operation == TokenKind::kPlus ||
                     binary.operation == TokenKind::kMinus ||
                     binary.operation == TokenKind::kStar)) {
    const std::string_view operation =
        binary.operation == TokenKind::kPlus    ? "add"
        : binary.operation == TokenKind::kMinus ? "sub"
                                                : "mul";
    const std::string aggregate = next_address();
    const std::string overflow = next_address();
    const std::string valid = next_address();
    output << "  " << aggregate << " = call { " << type << ", i1 } @llvm."
           << (is_unsigned ? 'u' : 's') << operation << ".with.overflow.i"
           << integer_properties->bit_width << '(' << type << ' '
           << value(binary.left) << ", " << type << ' ' << value(binary.right)
           << ")\n"
           << "  " << result_name(instruction) << " = extractvalue { " << type
           << ", i1 } " << aggregate << ", 0\n"
           << "  " << overflow << " = extractvalue { " << type << ", i1 } "
           << aggregate << ", 1\n"
           << "  " << valid << " = xor i1 " << overflow << ", true\n";
    emit_integer_arithmetic_guard(valid, kClothIntegerArithmeticOverflow,
                                  output);
    return;
  }
  if (is_integer && (binary.operation == TokenKind::kSlash ||
                     binary.operation == TokenKind::kPercent)) {
    const bool division = binary.operation == TokenKind::kSlash;
    const std::string nonzero = next_address();
    output << "  " << nonzero << " = icmp ne " << type << ' '
           << value(binary.right) << ", 0\n";
    emit_integer_arithmetic_guard(
        nonzero,
        division ? kClothIntegerDivisionByZero : kClothIntegerRemainderByZero,
        output);
    if (!is_unsigned) {
      const std::uint64_t minimum_magnitude =
          std::uint64_t{1} << (integer_properties->bit_width - 1);
      const std::string minimum = "-" + std::to_string(minimum_magnitude);
      const std::string left_is_minimum = next_address();
      const std::string right_is_negative_one = next_address();
      const std::string overflows = next_address();
      const std::string valid = next_address();
      output << "  " << left_is_minimum << " = icmp eq " << type << ' '
             << value(binary.left) << ", " << minimum << "\n"
             << "  " << right_is_negative_one << " = icmp eq " << type << ' '
             << value(binary.right) << ", -1\n"
             << "  " << overflows << " = and i1 " << left_is_minimum << ", "
             << right_is_negative_one << "\n"
             << "  " << valid << " = xor i1 " << overflows << ", true\n";
      emit_integer_arithmetic_guard(valid, kClothIntegerArithmeticOverflow,
                                    output);
    }
    output << "  " << result_name(instruction) << " = "
           << (division ? (is_unsigned ? "udiv" : "sdiv")
                        : (is_unsigned ? "urem" : "srem"))
           << ' ' << type << ' ' << value(binary.left) << ", "
           << value(binary.right) << '\n';
    return;
  }
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
    case TokenKind::kAmpersand:
      operation = "and";
      break;
    case TokenKind::kPipe:
      operation = "or";
      break;
    case TokenKind::kCaret:
      operation = "xor";
      break;
    case TokenKind::kShiftLeft:
    case TokenKind::kShiftRight: {
      const TypeId count_type = value_type(binary.right);
      const std::optional<NumericTypeProperties> value_properties =
          numeric_type_properties(kind);
      const std::optional<NumericTypeProperties> count_properties =
          numeric_type_properties(module_.semantics().type(count_type).kind);
      if (!value_properties || !count_properties ||
          value_properties->category == NumericCategory::kFloatingPoint ||
          count_properties->category == NumericCategory::kFloatingPoint) {
        module_.report(instruction.range,
                       "invalid shift reached LLVM lowering");
        return;
      }
      const std::string count_valid = next_address();
      const std::string valid_byte = next_address();
      output << "  " << count_valid << " = icmp ult "
             << module_.llvm_type(count_type) << ' ' << value(binary.right)
             << ", " << value_properties->bit_width << '\n'
             << "  " << valid_byte << " = zext i1 " << count_valid << " to i8\n"
             << "  call void @cloth_rt_require_shift_count(i8 " << valid_byte
             << ")\n";
      std::string count = value(binary.right);
      if (count_properties->bit_width != value_properties->bit_width) {
        const std::string converted = next_address();
        output << "  " << converted << " = "
               << (count_properties->bit_width < value_properties->bit_width
                       ? "zext"
                       : "trunc")
               << ' ' << module_.llvm_type(count_type) << ' ' << count << " to "
               << type << '\n';
        count = converted;
      }
      operation = binary.operation == TokenKind::kShiftLeft
                      ? "shl"
                      : (is_unsigned ? "lshr" : "ashr");
      output << "  " << result_name(instruction) << " = " << operation << ' '
             << type << ' ' << value(binary.left) << ", " << count << '\n';
      return;
    }
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

void BodyEmitter::emit_conversion(const MirInstruction& instruction,
                                  const MirConvertInstruction& conversion,
                                  std::ostringstream& output) {
  if (conversion.kind == MirConversionKind::kCheckedNumeric) {
    emit_checked_numeric_conversion(instruction, conversion, output);
    return;
  }
  if (conversion.kind != MirConversionKind::kWidenNumeric) {
    values_.at(instruction.result->value) = value(conversion.value);
    return;
  }

  const TypeId source_type = value_type(conversion.value);
  const TypeKind source_kind = module_.semantics().type(source_type).kind;
  const TypeKind target_kind = module_.semantics().type(instruction.type).kind;
  const std::optional<NumericTypeProperties> source =
      numeric_type_properties(source_kind);
  const std::optional<NumericTypeProperties> target =
      numeric_type_properties(target_kind);
  if (!source || !target || !can_widen_numeric(source_kind, target_kind)) {
    module_.report(instruction.range,
                   "invalid numeric widening reached LLVM lowering");
    return;
  }

  std::string_view operation;
  if (source->category == NumericCategory::kFloatingPoint) {
    operation = "fpext";
  } else if (source->category == NumericCategory::kSignedInteger) {
    operation = "sext";
  } else {
    operation = "zext";
  }
  output << "  " << result_name(instruction) << " = " << operation << ' '
         << module_.llvm_type(source_type) << ' ' << value(conversion.value)
         << " to " << module_.llvm_type(instruction.type) << '\n';
}

void BodyEmitter::emit_checked_numeric_conversion(
    const MirInstruction& instruction, const MirConvertInstruction& conversion,
    std::ostringstream& output) {
  const TypeId source_type = value_type(conversion.value);
  const TypeKind source_kind = module_.semantics().type(source_type).kind;
  const TypeKind target_kind = module_.semantics().type(instruction.type).kind;
  const std::optional<NumericTypeProperties> source =
      numeric_type_properties(source_kind);
  const std::optional<NumericTypeProperties> target =
      numeric_type_properties(target_kind);
  if (!source || !target) {
    module_.report(instruction.range,
                   "invalid checked numeric conversion reached LLVM lowering");
    return;
  }

  const std::string operand = value(conversion.value);
  const std::string source_llvm_type = module_.llvm_type(source_type);
  const std::string target_llvm_type = module_.llvm_type(instruction.type);
  const auto emit_binary_condition =
      [this, &output](std::string_view operation, std::string_view type,
                      std::string_view left, std::string_view right) {
        const std::string result = next_address();
        output << "  " << result << " = " << operation << ' ' << type << ' '
               << left << ", " << right << '\n';
        return result;
      };
  const auto combine_conditions = [&emit_binary_condition](
                                      std::optional<std::string>& current,
                                      std::string condition) {
    if (!current) {
      current = std::move(condition);
      return;
    }
    current = emit_binary_condition("and", "i1", *current, condition);
  };
  const auto require_condition =
      [this, &output](const std::optional<std::string>& condition) {
        if (!condition) {
          return;
        }
        const std::string widened = next_address();
        output << "  " << widened << " = zext i1 " << *condition << " to i8\n"
               << "  call void @cloth_rt_require_numeric_conversion(i8 "
               << widened << ")\n";
      };
  const auto floating_constant = [&source](long double value) {
    const std::optional<std::string> formatted =
        source->bit_width == 32
            ? format_floating_literal(static_cast<float>(value))
            : format_floating_literal(static_cast<double>(value));
    return formatted.value_or("0.000000e+00");
  };

  if (source->category != NumericCategory::kFloatingPoint &&
      target->category != NumericCategory::kFloatingPoint) {
    std::optional<std::string> valid;
    if (source->category == NumericCategory::kSignedInteger &&
        target->category == NumericCategory::kUnsignedInteger) {
      combine_conditions(
          valid,
          emit_binary_condition("icmp sge", source_llvm_type, operand, "0"));
      if (target->bit_width < source->bit_width) {
        const std::uint64_t maximum =
            (std::uint64_t{1} << target->bit_width) - 1;
        combine_conditions(
            valid, emit_binary_condition("icmp sle", source_llvm_type, operand,
                                         std::to_string(maximum)));
      }
    } else if (source->category == NumericCategory::kUnsignedInteger &&
               target->category == NumericCategory::kSignedInteger) {
      if (target->bit_width <= source->bit_width) {
        const std::uint64_t maximum =
            (std::uint64_t{1} << (target->bit_width - 1)) - 1;
        combine_conditions(
            valid, emit_binary_condition("icmp ule", source_llvm_type, operand,
                                         std::to_string(maximum)));
      }
    } else if (target->bit_width < source->bit_width) {
      if (source->category == NumericCategory::kSignedInteger) {
        const std::uint64_t magnitude = std::uint64_t{1}
                                        << (target->bit_width - 1);
        combine_conditions(
            valid, emit_binary_condition("icmp sge", source_llvm_type, operand,
                                         "-" + std::to_string(magnitude)));
        combine_conditions(
            valid, emit_binary_condition("icmp sle", source_llvm_type, operand,
                                         std::to_string(magnitude - 1)));
      } else {
        const std::uint64_t maximum =
            (std::uint64_t{1} << target->bit_width) - 1;
        combine_conditions(
            valid, emit_binary_condition("icmp ule", source_llvm_type, operand,
                                         std::to_string(maximum)));
      }
    }
    require_condition(valid);

    if (source->bit_width == target->bit_width) {
      values_.at(instruction.result->value) = operand;
      return;
    }
    const std::string_view operation =
        source->bit_width > target->bit_width
            ? "trunc"
            : (source->category == NumericCategory::kSignedInteger ? "sext"
                                                                   : "zext");
    output << "  " << result_name(instruction) << " = " << operation << ' '
           << source_llvm_type << ' ' << operand << " to " << target_llvm_type
           << '\n';
    return;
  }

  if (source->category != NumericCategory::kFloatingPoint) {
    const std::string_view operation =
        source->category == NumericCategory::kSignedInteger ? "sitofp"
                                                            : "uitofp";
    output << "  " << result_name(instruction) << " = " << operation << ' '
           << source_llvm_type << ' ' << operand << " to " << target_llvm_type
           << '\n';
    return;
  }

  if (target->category != NumericCategory::kFloatingPoint) {
    const std::string truncated = next_address();
    output << "  " << truncated << " = call " << source_llvm_type
           << " @llvm.trunc.f" << source->bit_width << '(' << source_llvm_type
           << ' ' << operand << ")\n";
    const long double upper = std::ldexp(
        1.0L,
        static_cast<int>(target->category == NumericCategory::kSignedInteger
                             ? target->bit_width - 1
                             : target->bit_width));
    const long double lower =
        target->category == NumericCategory::kSignedInteger ? -upper : 0.0L;
    std::optional<std::string> valid;
    combine_conditions(
        valid, emit_binary_condition("fcmp oge", source_llvm_type, truncated,
                                     floating_constant(lower)));
    combine_conditions(
        valid, emit_binary_condition("fcmp olt", source_llvm_type, truncated,
                                     floating_constant(upper)));
    require_condition(valid);
    const std::string_view operation =
        target->category == NumericCategory::kSignedInteger ? "fptosi"
                                                            : "fptoui";
    output << "  " << result_name(instruction) << " = " << operation << ' '
           << source_llvm_type << ' ' << truncated << " to " << target_llvm_type
           << '\n';
    return;
  }

  if (source->bit_width < target->bit_width) {
    output << "  " << result_name(instruction) << " = fpext "
           << source_llvm_type << ' ' << operand << " to " << target_llvm_type
           << '\n';
    return;
  }

  std::optional<std::string> valid;
  const double maximum = static_cast<double>(std::numeric_limits<float>::max());
  combine_conditions(
      valid, emit_binary_condition("fcmp oge", source_llvm_type, operand,
                                   floating_constant(-maximum)));
  combine_conditions(
      valid, emit_binary_condition("fcmp ole", source_llvm_type, operand,
                                   floating_constant(maximum)));
  const std::string bits = next_address();
  const std::string exponent = next_address();
  const std::string non_finite = next_address();
  output << "  " << bits << " = bitcast double " << operand << " to i64\n"
         << "  " << exponent << " = and i64 " << bits
         << ", 9218868437227405312\n"
         << "  " << non_finite << " = icmp eq i64 " << exponent
         << ", 9218868437227405312\n";
  const std::string range_or_non_finite =
      emit_binary_condition("or", "i1", *valid, non_finite);
  require_condition(range_or_non_finite);
  output << "  " << result_name(instruction) << " = fptrunc "
         << source_llvm_type << ' ' << operand << " to " << target_llvm_type
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
  } else if (type.kind == TypeKind::kInterface && type.file) {
    const FileSemantics& interface_file = module_.semantics().file(*type.file);
    if (!interface_file.interface_id) {
      module_.report(range, "interface checked target has no runtime identity");
      return;
    }
    output << "  " << raw << " = call i8 @cloth_rt_object_is_interface(ptr "
           << value(source) << ", i64 " << *interface_file.interface_id
           << ")\n";
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
      case IntrinsicKind::kPrintStruct: {
        const std::string display =
            "<" + module_.semantics().type(symbol.parameter_types[0]).name +
            ">";
        const std::string bytes = module_.add_string_literal(display);
        const std::string text = next_address();
        output << "  " << text << " = call ptr @cloth_rt_string_literal(ptr "
               << bytes << ", i64 " << display.size() << ")\n"
               << "  call void @cloth_rt_print(ptr " << text << ")\n";
        break;
      }
      case IntrinsicKind::kPrintEnum:
        output << "  call void "
               << module_.enum_printer(symbol.parameter_types[0]) << "(i32 "
               << argument << ")\n";
        break;
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
  std::vector<std::string> arguments;
  for (std::size_t index = 0; index < call.arguments.size(); ++index) {
    const TypeId type = value_type(call.arguments[index]);
    if (module_.is_aggregate(type)) {
      const auto slot = call_argument(instruction, index);
      copy_value(type, slot, value(call.arguments[index]), output);
      arguments.push_back(slot);
    } else {
      arguments.push_back(value(call.arguments[index]));
    }
  }
  const auto clear_arguments = [&] {
    for (std::size_t index = 0; index < call.arguments.size(); ++index)
      if (module_.is_aggregate(value_type(call.arguments[index])))
        zero_root(value_type(call.arguments[index]), arguments[index], output);
  };
  if (call.kind == MirCallKind::kBaseConstructor) {
    if (callable->kind != AbiCallableKind::kConstructor ||
        callable->initializer_mangled_name.empty() || instruction.result) {
      module_.report(instruction.range,
                     "invalid base constructor call reached LLVM lowering");
      return;
    }
    output << "  call void @" << callable->initializer_mangled_name << "(ptr "
           << receiver_;
    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
      const TypeId type = callable->parameters.at(index).type;
      output << ", " << module_.llvm_type(type) << ' ' << arguments[index];
    }
    output << ")\n";
    clear_arguments();
    return;
  }
  const bool has_receiver =
      callable->receiver_mode == AbiReceiverMode::kReference ||
      callable->receiver_mode == AbiReceiverMode::kReadOnlyValue;
  std::string receiver = receiver_;
  if (call.kind == MirCallKind::kClassQualified) {
    receiver = "null";
  } else if (call.receiver) {
    receiver = value(*call.receiver);
  }
  std::string target = "@" + callable->mangled_name;
  if (call.dispatch == MirDispatchKind::kVirtual) {
    if (!has_receiver || !symbol.virtual_slot) {
      module_.report(instruction.range,
                     "invalid virtual call reached LLVM lowering");
      return;
    }
    output << "  call void @cloth_rt_require_receiver(ptr " << receiver
           << ")\n";
    const std::string descriptor = next_address();
    const std::string virtuals_address = next_address();
    const std::string virtuals = next_address();
    const std::string slot_address = next_address();
    const std::string function = next_address();
    output << "  " << descriptor << " = load ptr, ptr " << receiver << "\n"
           << "  " << virtuals_address
           << " = getelementptr inbounds { i64, ptr, ptr, i64, i64, i64, ptr, "
              "i64, ptr, i64, ptr, i64 }, ptr "
           << descriptor << ", i32 0, i32 8\n"
           << "  " << virtuals << " = load ptr, ptr " << virtuals_address
           << "\n"
           << "  " << slot_address << " = getelementptr inbounds ptr, ptr "
           << virtuals << ", i64 " << *symbol.virtual_slot << "\n"
           << "  " << function << " = load ptr, ptr " << slot_address << "\n";
    target = function;
  } else if (call.dispatch == MirDispatchKind::kInterface) {
    if (!has_receiver || !call.interface_file || !call.interface_slot) {
      module_.report(instruction.range,
                     "invalid interface call reached LLVM lowering");
      return;
    }
    const FileSemantics& interface_file =
        module_.semantics().file(*call.interface_file);
    if (!interface_file.interface_id) {
      module_.report(instruction.range,
                     "interface call has no runtime identity");
      return;
    }
    const std::string function = next_address();
    output << "  " << function
           << " = call ptr @cloth_rt_interface_function(ptr " << receiver
           << ", i64 " << *interface_file.interface_id << ", i64 "
           << *call.interface_slot << ")\n";
    target = function;
  }
  if (instruction.result && callable->return_mode != AbiReturnMode::kIndirect) {
    output << "  " << result_name(instruction) << " = ";
  } else {
    output << "  ";
  }
  output << "call " << module_.return_type(callable->return_type) << ' '
         << target << '(';
  bool needs_comma = false;
  if (callable->return_mode == AbiReturnMode::kIndirect) {
    output << "ptr " << result_name(instruction);
    needs_comma = true;
  }
  if (has_receiver) {
    if (needs_comma) output << ", ";
    output << "ptr " << receiver;
    needs_comma = true;
  }
  for (std::size_t index = 0; index < call.arguments.size(); ++index) {
    if (needs_comma) {
      output << ", ";
    }
    const TypeId type = symbol.parameter_types.at(index);
    output << module_.llvm_type(type) << ' ' << arguments[index];
    needs_comma = true;
  }
  output << ")\n";
  clear_arguments();
}

bool BodyEmitter::has_aggregate_phi(std::size_t block) const {
  if (block >= body_.blocks.size()) return false;
  return std::ranges::any_of(
      body_.blocks[block].instructions, [&](const MirInstruction& instruction) {
        return std::holds_alternative<MirPhiInstruction>(instruction.data) &&
               module_.is_aggregate(instruction.type);
      });
}

bool BodyEmitter::needs_edge_block(std::size_t predecessor,
                                   std::size_t successor) const {
  if (has_aggregate_phi(successor)) return true;
  const auto& instructions = body_.blocks[successor].instructions;
  return std::holds_alternative<MirSwitchTerminator>(
             body_.blocks[predecessor].terminator.data) &&
         !instructions.empty() &&
         std::holds_alternative<MirPhiInstruction>(instructions.front().data);
}

std::string BodyEmitter::edge_label(std::size_t predecessor,
                                    std::size_t successor) const {
  if (!needs_edge_block(predecessor, successor))
    return "bb" + std::to_string(successor);
  return "edge." + std::to_string(predecessor) + "." +
         std::to_string(successor);
}

void BodyEmitter::emit_phi_edges(std::ostringstream& output) {
  for (std::size_t predecessor = 0; predecessor < body_.blocks.size();
       ++predecessor) {
    // One bridge per unique successor also gives scalar phis one LLVM edge,
    // regardless of how many switch labels (or guarded defaults) select it.
    for (const auto successor : successors_[predecessor]) {
      if (!needs_edge_block(predecessor, successor.value)) continue;
      output << "edge." << predecessor << "." << successor.value << ":\n";
      // Two phases preserve simultaneous phi assignment, including loop swaps.
      for (const auto& instruction :
           body_.blocks[successor.value].instructions) {
        const auto* phi = std::get_if<MirPhiInstruction>(&instruction.data);
        if (!phi || !module_.is_aggregate(instruction.type)) continue;
        for (const auto& incoming : phi->incoming) {
          if (incoming.predecessor.value == predecessor)
            copy_value(instruction.type,
                       "%phi.copy." + std::to_string(instruction.result->value),
                       value(incoming.value), output);
        }
      }
      for (const auto& instruction :
           body_.blocks[successor.value].instructions) {
        if (!std::holds_alternative<MirPhiInstruction>(instruction.data) ||
            !module_.is_aggregate(instruction.type))
          continue;
        const auto scratch =
            "%phi.copy." + std::to_string(instruction.result->value);
        copy_value(instruction.type, result_name(instruction), scratch, output);
        zero_root(instruction.type, scratch, output);
      }
      output << "  br label %bb" << successor.value << '\n';
    }
  }
}

void BodyEmitter::emit_phi(const MirInstruction& instruction,
                           const MirPhiInstruction& phi,
                           std::ostringstream& output) {
  if (module_.is_aggregate(instruction.type)) return;
  output << "  " << result_name(instruction) << " = phi "
         << module_.llvm_type(instruction.type) << ' ';
  for (std::size_t index = 0; index < phi.incoming.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << "[ " << value(phi.incoming[index].value) << ", %"
           << (needs_edge_block(phi.incoming[index].predecessor.value,
                                current_block_)
                   ? edge_label(phi.incoming[index].predecessor.value,
                                current_block_)
                   : "bb" +
                         std::to_string(phi.incoming[index].predecessor.value))
           << " ]";
  }
  output << '\n';
}

void BodyEmitter::emit_terminator(const MirTerminator& terminator,
                                  bool is_reachable,
                                  std::ostringstream& output) {
  if (const auto* jump = std::get_if<MirJumpTerminator>(&terminator.data)) {
    output << "  br label %" << edge_label(current_block_, jump->target.value)
           << '\n';
  } else if (const auto* branch =
                 std::get_if<MirBranchTerminator>(&terminator.data)) {
    output << "  br i1 " << value(branch->condition) << ", label %"
           << edge_label(current_block_, branch->then_block.value)
           << ", label %"
           << edge_label(current_block_, branch->else_block.value) << '\n';
  } else if (const auto* selection =
                 std::get_if<MirSwitchTerminator>(&terminator.data)) {
    const std::string type = module_.llvm_type(selection->selector_type);
    const bool guard_default =
        selection->invalid_block &&
        *selection->invalid_block != selection->default_block;
    const std::string guard =
        "switch.default." + std::to_string(current_block_);
    output << "  switch " << type << ' ' << value(selection->selector)
           << ", label %"
           << (guard_default
                   ? guard
                   : edge_label(current_block_, selection->default_block.value))
           << " [\n";
    for (const auto& entry : selection->cases)
      output << "    " << type << ' ' << entry.value.bits << ", label %"
             << edge_label(current_block_, entry.target.value) << '\n';
    output << "  ]\n";
    if (guard_default) {
      const auto& enum_type =
          module_.semantics().type(selection->selector_type);
      const auto count =
          module_.semantics().file(*enum_type.file).enum_cases.size();
      const std::string valid = next_address();
      output << guard << ":\n  " << valid << " = icmp ult " << type << ' '
             << value(selection->selector) << ", " << count << "\n  br i1 "
             << valid << ", label %"
             << edge_label(current_block_, selection->default_block.value)
             << ", label %"
             << edge_label(current_block_, selection->invalid_block->value)
             << '\n';
    }
  } else if (std::holds_alternative<MirTrapTerminator>(terminator.data)) {
    output << "  call void @llvm.trap()\n  unreachable\n";
  } else if (const auto* return_terminator =
                 std::get_if<MirReturnTerminator>(&terminator.data)) {
    if (return_terminator->value && module_.is_aggregate(return_type_))
      copy_value(return_type_, "%result", value(*return_terminator->value),
                 output);
    if (is_reachable) {
      emit_gc_epilogue(output);
    }
    if (is_constructor_) {
      output << (allocates_constructor_ ? "  ret ptr %self\n" : "  ret void\n");
    } else if (module_.is_aggregate(return_type_)) {
      output << "  ret void\n";
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
  if (symbol == module_.semantics().file(file_.file).self_symbol)
    return receiver_;
  if (is_aggregate_parameter(symbol)) return argument_name(symbol);
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
  if (module_.is_aggregate(value_type(value))) return this->value(value);
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
  return !is_constructor_ && !has_struct_receiver() && receiver_ != "undef";
}

std::size_t BodyEmitter::gc_root_count() const noexcept {
  std::size_t count = gc_value_root_count_ + (has_receiver_root() ? 1U : 0U) +
                      (is_managed_constructor() ? 1U : 0U);
  for (const SymbolId symbol : gc_symbol_roots_)
    count += module_.abi()
                 .types.at(module_.semantics().symbol(symbol).type.value)
                 .reference_offsets.size();
  for (const auto& slot : argument_slots_)
    count += module_.abi().types.at(slot.type.value).reference_offsets.size();
  if (has_struct_receiver())
    count += module_.abi()
                 .types.at(module_.semantics().file(file_.file).type.value)
                 .reference_offsets.size();
  return count;
}

std::string BodyEmitter::next_address() {
  return "%addr" + std::to_string(address_count_++);
}

}  // namespace

const AbiCallable* find_native_entry_point(
    const AbiModule& abi, const SemanticModel& semantics,
    DiagnosticEngine& diagnostics, std::optional<std::string_view> entry_file) {
  const AbiCallable* entry = nullptr;
  SourceRange entry_range = point_range(SourceLocation{"<entry>", 0, 1, 1});
  bool saw_main = false;
  for (const AbiFileClass& file : abi.files) {
    const SemanticSymbol& file_symbol =
        semantics.symbol(semantics.file(file.file).symbol);
    if (entry_file && file_symbol.name != *entry_file) {
      continue;
    }
    entry_range = file_symbol.range;
    for (const AbiCallable& callable : file.functions) {
      const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
      if (symbol.name != "Main") {
        continue;
      }
      saw_main = true;
      entry_range = symbol.range;
      const TypeKind return_kind = semantics.type(callable.return_type).kind;
      if (!symbol.is_static || !symbol.parameter_types.empty() ||
          callable.linkage != AbiLinkage::kExternal ||
          (return_kind != TypeKind::kVoid && return_kind != TypeKind::kInt32)) {
        continue;
      }
      if (entry != nullptr) {
        diagnostics.error(
            symbol.range,
            "native program has more than one eligible 'Main' function");
        return nullptr;
      }
      entry = &callable;
    }
  }
  if (entry == nullptr) {
    diagnostics.error(
        entry_range,
        saw_main ? "entry point 'Main' must be public and static, take no "
                   "parameters, and return no value or int32"
                 : "native program requires a public static 'Main' function");
  }
  return entry;
}

std::optional<LlvmIrModule> emit_llvm_ir(const MirModule& mir,
                                         const AbiModule& abi,
                                         const SemanticModel& semantics,
                                         DiagnosticEngine& diagnostics,
                                         LlvmIrOptions options) {
  return ModuleEmitter{mir, abi, semantics, diagnostics, options}.emit();
}

}  // namespace cloth
