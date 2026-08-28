#include "cloth/abi/abi_verifier.h"

#include "cloth/abi/abi.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>

namespace cloth {
namespace {

class AbiVerifier {
 public:
  AbiVerifier(const AbiModule& abi, const MirModule& mir,
              const SemanticModel& semantics, DiagnosticEngine& diagnostics)
      : abi_(abi),
        mir_(mir),
        semantics_(semantics),
        diagnostics_(diagnostics) {}

  bool run() {
    if (!is_valid_data_layout(abi_.target)) {
      report(fallback_range(), "target data layout is invalid");
      return false;
    }
    const AbiModule expected = lower_to_abi(mir_, semantics_, abi_.target);
    verify_types(expected);
    verify_files(expected);
    verify_unique_names();
    verify_calls();
    return is_valid_;
  }

 private:
  void verify_types(const AbiModule& expected) {
    if (abi_.types.size() != expected.types.size()) {
      report(fallback_range(), "type count does not match semantics");
    }
    const std::size_t count =
        std::min(abi_.types.size(), expected.types.size());
    for (std::size_t index = 0; index < count; ++index) {
      const AbiTypeLayout& type = abi_.types[index];
      if (type != expected.types[index]) {
        report(fallback_range(), "type layout does not match semantics");
      }
      if (type.type != TypeId{index}) {
        report(fallback_range(), "type layout order is unstable");
      }
      if (!is_power_of_two(type.storage.alignment)) {
        report(fallback_range(), "type alignment is not a power of two");
      }
    }
  }

  void verify_files(const AbiModule& expected) {
    if (abi_.files.size() != expected.files.size()) {
      report(fallback_range(), "file count does not match MIR");
    }
    const std::size_t count =
        std::min(abi_.files.size(), expected.files.size());
    for (std::size_t index = 0; index < count; ++index) {
      verify_file(abi_.files[index], expected.files[index], index);
    }
  }

  void verify_file(const AbiFileClass& file, const AbiFileClass& expected,
                   std::size_t index) {
    const SourceRange range = symbol_range(file.symbol);
    if (file.file != FileId{index} || file.file != expected.file ||
        file.symbol != expected.symbol) {
      report(range, "file identity does not match MIR");
    }
    if (file.base_file != expected.base_file) {
      report(range, "file-class base does not match MIR");
    }
    if (file.layout != expected.layout) {
      report(range, "class layout does not match its fields and target");
    }
    if (file.type_descriptor != expected.type_descriptor) {
      report(range, "type descriptor does not match its class layout");
    }
    if (file.static_fields != expected.static_fields) {
      report(range, "static field ABI does not match semantics");
    }
    if (file.member_order != expected.member_order) {
      report(range, "member order does not match MIR");
    }
    verify_class_layout(file.layout, range);
    verify_type_descriptor(file.type_descriptor, file.layout, range);
    verify_inheritance(file, range);
    verify_callables(file.functions, expected.functions, range, "function");
    verify_callables(file.constructors, expected.constructors, range,
                     "constructor");
  }

  void verify_inheritance(const AbiFileClass& file, SourceRange range) {
    if (!file.base_file) {
      if (file.type_descriptor.parent_file) {
        report(range, "root file class has a parent type descriptor");
      }
      return;
    }
    if (file.type_descriptor.parent_file != file.base_file) {
      report(range, "type descriptor parent does not match the base class");
    }
    if (file.base_file->value >= abi_.files.size() ||
        *file.base_file == file.file) {
      report(range, "file class has an invalid ABI base");
      return;
    }
    const AbiClassLayout& base = abi_.files[file.base_file->value].layout;
    if (file.layout.header_size != base.header_size ||
        file.layout.size < base.size ||
        file.layout.alignment < base.alignment ||
        file.layout.fields.size() < base.fields.size() ||
        !std::equal(base.fields.begin(), base.fields.end(),
                    file.layout.fields.begin())) {
      report(range, "derived class does not preserve its base layout prefix");
    }
  }

  void verify_type_descriptor(const AbiTypeDescriptor& descriptor,
                              const AbiClassLayout& layout, SourceRange range) {
    if (descriptor.kind != AbiHeapObjectKind::kFileClass ||
        descriptor.name.empty() || descriptor.size != layout.size ||
        descriptor.alignment != layout.alignment) {
      report(range, "file-class type descriptor is invalid");
    }
    std::uint64_t previous_offset = 0;
    bool has_previous_offset = false;
    for (const std::uint64_t offset : descriptor.reference_offsets) {
      if (offset < layout.header_size || offset > layout.size ||
          abi_.target.pointer.size > layout.size - offset ||
          offset % abi_.target.pointer.alignment != 0 ||
          (has_previous_offset && offset <= previous_offset)) {
        report(range, "type descriptor has an invalid reference offset");
        return;
      }
      previous_offset = offset;
      has_previous_offset = true;
    }
    for (std::size_t slot = 0; slot < descriptor.virtual_functions.size();
         ++slot) {
      const SymbolId symbol_id = descriptor.virtual_functions[slot];
      if (symbol_id.value >= semantics_.symbols().size()) {
        report(range, "type descriptor has an unknown virtual function");
        continue;
      }
      const SemanticSymbol& symbol = semantics_.symbol(symbol_id);
      if (symbol.kind != SymbolKind::kFunction || symbol.is_static ||
          symbol.visibility != Visibility::kPublic ||
          symbol.virtual_slot != slot) {
        report(range, "type descriptor has an invalid virtual function");
      }
    }
  }

  void verify_class_layout(const AbiClassLayout& layout, SourceRange range) {
    if (!is_power_of_two(layout.alignment)) {
      report(range, "class alignment is not a power of two");
      return;
    }
    if (layout.header_size > layout.size ||
        layout.size % layout.alignment != 0) {
      report(range, "class size is inconsistent with its alignment");
    }
    std::uint64_t previous_end = layout.header_size;
    for (const AbiFieldLayout& field : layout.fields) {
      if (field.type.value >= abi_.types.size()) {
        report(range, "field references an unknown ABI type");
        continue;
      }
      const AbiTypeLayout& type = abi_.types[field.type.value];
      if (!is_power_of_two(type.storage.alignment)) {
        report(range, "field type has an invalid alignment");
        continue;
      }
      if (field.offset < previous_end ||
          field.offset % type.storage.alignment != 0 ||
          field.offset > layout.size ||
          type.storage.size > layout.size - field.offset) {
        report(range, "field offset is outside its class layout");
      } else {
        previous_end = field.offset + type.storage.size;
      }
    }
  }

  void verify_callables(const std::vector<AbiCallable>& callables,
                        const std::vector<AbiCallable>& expected,
                        SourceRange range, std::string_view kind) {
    if (callables.size() != expected.size()) {
      report(range, std::string{kind} + " count does not match MIR");
    }
    const std::size_t count = std::min(callables.size(), expected.size());
    for (std::size_t index = 0; index < count; ++index) {
      if (callables[index] != expected[index]) {
        report(symbol_range(callables[index].symbol),
               std::string{kind} + " ABI does not match semantics");
      }
    }
  }

  void verify_unique_names() {
    std::unordered_set<std::string> names;
    for (const AbiFileClass& file : abi_.files) {
      for (const AbiStaticField& field : file.static_fields) {
        if (!names.insert(field.mangled_name).second) {
          report(symbol_range(field.symbol),
                 "mangled static field name is not unique");
        }
      }
      for (const AbiCallable& callable : file.functions) {
        verify_unique_name(callable, names);
      }
      for (const AbiCallable& callable : file.constructors) {
        verify_unique_name(callable, names);
        if (callable.initializer_mangled_name.empty() ||
            !names.insert(callable.initializer_mangled_name).second) {
          report(symbol_range(callable.symbol),
                 "constructor initializer name is empty or not unique");
        }
      }
    }
  }

  void verify_unique_name(const AbiCallable& callable,
                          std::unordered_set<std::string>& names) {
    if (!names.insert(callable.mangled_name).second) {
      report(symbol_range(callable.symbol),
             "mangled callable name is not unique");
    }
  }

  void verify_calls() {
    for (const MirFileClass& file : mir_.files) {
      for (const MirField& field : file.fields) {
        if (field.initializer) {
          verify_body_calls(*field.initializer);
        }
      }
      for (const MirCallable& callable : file.functions) {
        verify_body_calls(callable.body);
      }
      for (const MirCallable& callable : file.constructors) {
        verify_body_calls(callable.body);
      }
    }
  }

  void verify_body_calls(const MirBody& body) {
    for (const MirBasicBlock& block : body.blocks) {
      for (const MirInstruction& instruction : block.instructions) {
        const auto* call = std::get_if<MirCallInstruction>(&instruction.data);
        if (call == nullptr) {
          continue;
        }
        const SemanticSymbol& symbol = semantics_.symbol(call->callable);
        if (symbol.intrinsic != IntrinsicKind::kNone) {
          if (symbol.parameter_types.size() != call->arguments.size()) {
            report(instruction.range,
                   "MIR intrinsic call has the wrong parameter count");
          }
          if (symbol.type != instruction.type) {
            report(instruction.range,
                   "MIR intrinsic call has the wrong return type");
          }
          continue;
        }
        const AbiCallable* callable = find_callable(call->callable);
        if (callable == nullptr) {
          report(instruction.range, "MIR call has no ABI declaration");
          continue;
        }
        const bool is_base_initializer =
            call->kind == MirCallKind::kBaseConstructor;
        if (is_base_initializer &&
            (callable->kind != AbiCallableKind::kConstructor ||
             callable->initializer_mangled_name.empty())) {
          report(instruction.range,
                 "MIR base call has no constructor initializer ABI");
        }
        const bool has_receiver =
            !callable->parameters.empty() &&
            callable->parameters.front().kind == AbiParameterKind::kReceiver;
        const std::size_t receiver_count = has_receiver ? 1U : 0U;
        if (callable->parameters.size() !=
            call->arguments.size() + receiver_count) {
          report(instruction.range,
                 "MIR call does not match its ABI parameter count");
        }
        const TypeId expected_return = is_base_initializer
                                           ? semantics_.void_type()
                                           : callable->return_type;
        if (expected_return != instruction.type) {
          report(instruction.range,
                 "MIR call does not match its ABI return type");
        }
      }
    }
  }

  const AbiCallable* find_callable(SymbolId symbol) const {
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

  SourceRange symbol_range(SymbolId symbol) const {
    if (symbol.value < semantics_.symbols().size()) {
      return semantics_.symbol(symbol).range;
    }
    return fallback_range();
  }

  static SourceRange fallback_range() noexcept {
    return point_range(SourceLocation{"<abi>", 0, 1, 1});
  }

  void report(SourceRange range, std::string message) {
    diagnostics_.error(range, "internal ABI verification error: " + message);
    is_valid_ = false;
  }

  const AbiModule& abi_;
  const MirModule& mir_;
  const SemanticModel& semantics_;
  DiagnosticEngine& diagnostics_;
  bool is_valid_{true};
};

}  // namespace

bool verify_abi(const AbiModule& abi, const MirModule& mir,
                const SemanticModel& semantics, DiagnosticEngine& diagnostics) {
  return AbiVerifier{abi, mir, semantics, diagnostics}.run();
}

}  // namespace cloth
