// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi.h"

#include "cloth/abi/aggregate_limits.h"
#include "cloth/mir/mir.h"
#include "cloth/sema/canonical_identity.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/sema/visibility.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloth {

std::string_view abi_passing_mode_name(AbiPassingMode mode) {
  switch (mode) {
    case AbiPassingMode::kDirect:
      return "direct";
    case AbiPassingMode::kValuePointer:
      return "value_pointer";
    case AbiPassingMode::kResultPointer:
      return "result_pointer";
  }
  return "invalid";
}

std::string_view abi_return_mode_name(AbiReturnMode mode) {
  switch (mode) {
    case AbiReturnMode::kVoid:
      return "void";
    case AbiReturnMode::kDirect:
      return "direct";
    case AbiReturnMode::kIndirect:
      return "indirect";
  }
  return "invalid";
}

std::string_view abi_receiver_mode_name(AbiReceiverMode mode) {
  switch (mode) {
    case AbiReceiverMode::kNone:
      return "none";
    case AbiReceiverMode::kReference:
      return "reference";
    case AbiReceiverMode::kReadOnlyValue:
      return "readonly_value";
    case AbiReceiverMode::kConstruction:
      return "construction";
  }
  return "invalid";
}

namespace {

std::optional<std::uint64_t> checked_add(std::uint64_t left,
                                         std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::nullopt;
  }
  return left + right;
}

std::optional<std::uint64_t> checked_align(std::uint64_t value,
                                           std::uint64_t alignment) {
  if (!is_power_of_two(alignment)) return std::nullopt;
  const auto padded = checked_add(value, alignment - 1);
  if (!padded) return std::nullopt;
  return *padded & ~(alignment - 1);
}

std::uint32_t pointer_bit_width(const TargetDataLayout& target) noexcept {
  // The target is validated before lowering types.
  return static_cast<std::uint32_t>(target.pointer.size * 8);
}

AbiTypeLayout make_type_layout(TypeId type, AbiTypeKind kind,
                               std::uint32_t bit_width, std::uint64_t size,
                               std::uint64_t alignment) {
  return AbiTypeLayout{type, kind, bit_width, SizeAlignment{size, alignment},
                       kind == AbiTypeKind::kReference
                           ? std::vector<std::uint64_t>{0}
                           : std::vector<std::uint64_t>{}};
}

AbiTypeLayout lower_type(TypeId type, const SemanticType& semantic_type,
                         const TargetDataLayout& target) {
  switch (semantic_type.kind) {
    case TypeKind::kError:
      return make_type_layout(type, AbiTypeKind::kInvalid, 0, 0, 1);
    case TypeKind::kStruct:
      return make_type_layout(type, AbiTypeKind::kAggregate, 0, 0, 1);
    case TypeKind::kVoid:
      return make_type_layout(type, AbiTypeKind::kVoid, 0, 0, 1);
    case TypeKind::kNull:
    case TypeKind::kString:
    case TypeKind::kObject:
    case TypeKind::kFileClass:
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kNullable:
      return make_type_layout(type, AbiTypeKind::kReference,
                              pointer_bit_width(target), target.pointer.size,
                              target.pointer.alignment);
    case TypeKind::kBool:
      return make_type_layout(type, AbiTypeKind::kInteger, 1, 1, 1);
    case TypeKind::kChar:
      return make_type_layout(type, AbiTypeKind::kInteger, 32, 4, 4);
    case TypeKind::kByte:
    case TypeKind::kInt8:
    case TypeKind::kUint8:
      return make_type_layout(type, AbiTypeKind::kInteger, 8, 1, 1);
    case TypeKind::kInt16:
    case TypeKind::kUint16:
      return make_type_layout(type, AbiTypeKind::kInteger, 16, 2, 2);
    case TypeKind::kInt32:
    case TypeKind::kUint32:
    case TypeKind::kEnum:
      return make_type_layout(type, AbiTypeKind::kInteger, 32, 4, 4);
    case TypeKind::kInt64:
    case TypeKind::kUint64:
      return make_type_layout(type, AbiTypeKind::kInteger, 64, 8,
                              target.int64_alignment);
    case TypeKind::kFloat32:
      return make_type_layout(type, AbiTypeKind::kFloat, 32, 4, 4);
    case TypeKind::kFloat64:
      return make_type_layout(type, AbiTypeKind::kFloat, 64, 8,
                              target.float64_alignment);
  }
  return make_type_layout(type, AbiTypeKind::kInvalid, 0, 0, 1);
}

AbiLinkage lower_linkage(Visibility visibility) noexcept {
  return visibility == Visibility::kPublic ? AbiLinkage::kExternal
                                           : AbiLinkage::kInternal;
}

struct FileLayout {
  AbiClassLayout storage;
  std::vector<std::uint64_t> references;
  std::size_t depth;
};

std::optional<FileLayout> lower_file_layout(
    const MirFileClass& file, const SemanticModel& semantics,
    const std::vector<AbiTypeLayout>& types, const TargetDataLayout& target,
    const std::vector<std::optional<FileLayout>>& layouts,
    DiagnosticEngine& diagnostics) {
  const FileSemantics& semantic = semantics.file(file.file);
  const auto fail = [&](std::string message) -> std::optional<FileLayout> {
    diagnostics.error(semantics.symbol(file.symbol).range, std::move(message));
    return std::nullopt;
  };
  if (semantic.kind == FileTypeKind::kEnum) {
    return FileLayout{AbiClassLayout{0, 0, 1, {}}, {}, 0};
  }
  const bool aggregate = semantic.kind == FileTypeKind::kStruct;
  const std::uint64_t header =
      aggregate ? 0 : target.pointer.size * target.object_header_words;
  std::uint64_t offset = header;
  std::uint64_t alignment = aggregate ? 1 : target.pointer.alignment;
  std::size_t depth = aggregate ? 1 : 0;
  std::vector<AbiFieldLayout> fields;
  std::vector<std::uint64_t> references;
  if (file.base_file) {
    const auto& base = *layouts.at(file.base_file->value);
    offset = base.storage.size;
    alignment = std::max(alignment, base.storage.alignment);
    fields = base.storage.fields;
    references = base.references;
  }
  std::size_t instance_count = 0;
  for (const MirField& field : file.fields) {
    const SemanticSymbol& symbol = semantics.symbol(field.symbol);
    if (symbol.is_static) continue;
    if (aggregate && ++instance_count > kMaxStructFields) {
      return fail("struct exceeds the instance-field limit of 65536");
    }
    const AbiTypeLayout& type = types.at(symbol.type.value);
    if (type.kind == AbiTypeKind::kInvalid || type.kind == AbiTypeKind::kVoid ||
        type.storage.size == 0 || !is_power_of_two(type.storage.alignment)) {
      return fail("field has no valid ABI storage layout");
    }
    const auto aligned = checked_align(offset, type.storage.alignment);
    if (!aligned) return fail("field alignment overflows the target layout");
    const auto end = checked_add(*aligned, type.storage.size);
    if (!end) return fail("field size overflows the target layout");
    alignment = std::max(alignment, type.storage.alignment);
    fields.push_back(AbiFieldLayout{field.symbol, symbol.type, *aligned});
    if (type.reference_offsets.size() >
        kMaxLayoutReferences - references.size()) {
      return fail("layout exceeds the reference-slot limit of 65536");
    }
    for (const std::uint64_t reference : type.reference_offsets) {
      const auto shifted = checked_add(*aligned, reference);
      if (!shifted || reference > type.storage.size ||
          target.pointer.size > type.storage.size - reference ||
          *shifted % target.pointer.alignment != 0) {
        return fail("field has an invalid reference-slot layout");
      }
      references.push_back(*shifted);
    }
    offset = *end;
    if (aggregate && type.kind == AbiTypeKind::kAggregate) {
      const auto owner = semantics.type(symbol.type).file;
      depth = std::max(depth, layouts.at(owner->value)->depth + 1);
      if (depth > kMaxStructDepth) {
        return fail("struct exceeds the inline nesting limit of 128");
      }
    }
  }
  const auto size = checked_align(
      aggregate ? std::max(offset, std::uint64_t{1}) : offset, alignment);
  if (!size) return fail("padded size overflows the target layout");
  if (aggregate && *size > kMaxStructSize) {
    return fail("struct exceeds the padded-size limit of 1048576 bytes");
  }
  return FileLayout{AbiClassLayout{header, *size, alignment, std::move(fields)},
                    std::move(references), depth};
}

AbiTypeDescriptor lower_type_descriptor(
    const AbiClassLayout& layout, const SemanticSymbol& class_symbol,
    std::vector<std::uint64_t> reference_offsets,
    std::optional<FileId> parent_file, const FileSemantics& file,
    const SemanticModel& semantics) {
  std::vector<AbiTypeDescriptor::InterfaceDispatch> interfaces;
  interfaces.reserve(file.interface_implementations.size());
  for (const InterfaceImplementation& implementation :
       file.interface_implementations) {
    const FileSemantics& interface_file =
        semantics.file(implementation.interface_file);
    interfaces.push_back(AbiTypeDescriptor::InterfaceDispatch{
        implementation.interface_file, interface_file.interface_id.value_or(0),
        implementation.functions});
  }
  std::ranges::sort(interfaces, {},
                    &AbiTypeDescriptor::InterfaceDispatch::interface_id);
  return AbiTypeDescriptor{
      AbiHeapObjectKind::kFileClass,
      parent_file,
      class_symbol.name,
      layout.size,
      layout.alignment,
      std::move(reference_offsets),
      file.virtual_functions,
      std::move(interfaces),
      file.kind == FileTypeKind::kClass
          ? mangle_canonical_identity(canonical_member_identity(
                file.identity, CanonicalMemberKind::kDescriptor, ""))
          : std::string{}};
}

AbiCallable lower_callable(const MirCallable& callable, AbiCallableKind kind,
                           const FileSemantics& file,
                           const SemanticModel& semantics) {
  const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
  const bool constructor = kind == AbiCallableKind::kConstructor;
  const bool struct_owner = file.kind == FileTypeKind::kStruct;
  const TypeId return_type = constructor ? file.type : symbol.type;
  const TypeKind result_kind = semantics.type(return_type).kind;
  const AbiReturnMode return_mode =
      result_kind == TypeKind::kStruct ? AbiReturnMode::kIndirect
      : result_kind == TypeKind::kVoid ? AbiReturnMode::kVoid
                                       : AbiReturnMode::kDirect;
  AbiReceiverMode receiver_mode = AbiReceiverMode::kNone;
  std::vector<AbiParameter> parameters;
  parameters.reserve(callable.parameters.size() + 2);
  if (return_mode == AbiReturnMode::kIndirect) {
    parameters.push_back({AbiParameterKind::kResult, std::nullopt, return_type,
                          AbiPassingMode::kResultPointer});
  }
  if (constructor && struct_owner) {
    receiver_mode = AbiReceiverMode::kConstruction;
  } else if (!constructor && !symbol.is_static) {
    receiver_mode = struct_owner ? AbiReceiverMode::kReadOnlyValue
                                 : AbiReceiverMode::kReference;
    parameters.push_back({AbiParameterKind::kReceiver, file.self_symbol,
                          file.type,
                          struct_owner ? AbiPassingMode::kValuePointer
                                       : AbiPassingMode::kDirect});
  }
  for (const SymbolId parameter : callable.parameters) {
    const TypeId type = semantics.symbol(parameter).type;
    parameters.push_back({AbiParameterKind::kExplicit, parameter, type,
                          semantics.type(type).kind == TypeKind::kStruct
                              ? AbiPassingMode::kValuePointer
                              : AbiPassingMode::kDirect});
  }
  const bool initializer = constructor && !struct_owner;
  return AbiCallable{
      callable.symbol,
      kind,
      lower_linkage(symbol.visibility),
      AbiCallingConvention::kC,
      mangle_abi_symbol(symbol, semantics),
      initializer ? mangle_abi_constructor_initializer(symbol, semantics)
                  : std::string{},
      return_type,
      std::move(parameters),
      initializer ? lower_linkage(symbol.visibility) : AbiLinkage::kInternal,
      return_mode,
      receiver_mode};
}

}  // namespace

std::string mangle_abi_symbol(const SemanticSymbol& symbol,
                              const SemanticModel& semantics) {
  return mangle_canonical_identity(
      canonical_symbol_identity(symbol, semantics,
                                symbol.kind == SymbolKind::kConstructor
                                    ? CanonicalMemberKind::kConstructor
                                    : CanonicalMemberKind::kFunction));
}

std::string mangle_abi_static_field(const SemanticSymbol& symbol,
                                    const SemanticModel& semantics) {
  return mangle_canonical_identity(canonical_symbol_identity(
      symbol, semantics, CanonicalMemberKind::kStaticField));
}

std::string mangle_abi_constructor_initializer(const SemanticSymbol& symbol,
                                               const SemanticModel& semantics) {
  return mangle_canonical_identity(canonical_symbol_identity(
      symbol, semantics, CanonicalMemberKind::kConstructorInitializer));
}

std::optional<AbiModule> lower_to_abi(const MirModule& mir,
                                      const SemanticModel& semantics,
                                      TargetDataLayout target,
                                      DiagnosticEngine& diagnostics) {
  const auto fail = [&](std::string message) -> std::optional<AbiModule> {
    diagnostics.error(point_range(SourceLocation{"<abi>", 0, 1, 1}),
                      std::move(message));
    return std::nullopt;
  };
  if (!is_valid_data_layout(target) ||
      target.pointer.size % target.pointer.alignment != 0) {
    return fail("target data layout is invalid");
  }
  AbiModule abi{std::move(target), {}, {}};
  abi.types.reserve(semantics.types().size());
  for (std::size_t index = 0; index < semantics.types().size(); ++index) {
    const TypeId type{index};
    abi.types.push_back(lower_type(type, semantics.type(type), abi.target));
  }

  // Kahn traversal avoids host recursion and repeatedly rescanning long chains.
  std::vector<std::vector<std::size_t>> dependents(mir.files.size());
  std::vector<std::size_t> pending(mir.files.size(), 0);
  std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<>>
      ready;
  for (std::size_t index = 0; index < mir.files.size(); ++index) {
    const MirFileClass& file = mir.files[index];
    if (file.file != FileId{index} || index >= semantics.files().size()) {
      return fail("file identity is invalid during ABI lowering");
    }
    std::vector<std::size_t> dependencies;
    if (file.base_file) dependencies.push_back(file.base_file->value);
    for (const MirField& field : file.fields) {
      const SemanticSymbol& symbol = semantics.symbol(field.symbol);
      if (symbol.is_static) continue;
      const SemanticType& type = semantics.type(symbol.type);
      if (type.kind == TypeKind::kStruct) {
        if (!type.file) return fail("struct field has no nominal owner");
        dependencies.push_back(type.file->value);
      }
    }
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
                       dependencies.end());
    pending[index] = dependencies.size();
    for (const std::size_t dependency : dependencies) {
      if (dependency >= mir.files.size()) {
        return fail("layout dependency has no file declaration");
      }
      dependents[dependency].push_back(index);
    }
    if (dependencies.empty()) ready.push(index);
  }
  std::vector<std::optional<FileLayout>> layouts(mir.files.size());
  std::size_t completed = 0;
  std::size_t map_entries = 0;
  while (!ready.empty()) {
    const std::size_t index = ready.top();
    ready.pop();
    const MirFileClass& file = mir.files[index];
    auto layout = lower_file_layout(file, semantics, abi.types, abi.target,
                                    layouts, diagnostics);
    if (!layout) return std::nullopt;
    if (layout->references.size() > kMaxAggregateMapEntries - map_entries) {
      diagnostics.error(
          semantics.symbol(file.symbol).range,
          "compilation exceeds the aggregate reference-map limit");
      return std::nullopt;
    }
    map_entries += layout->references.size();
    if (semantics.file(file.file).kind == FileTypeKind::kStruct) {
      AbiTypeLayout& type = abi.types.at(semantics.file(file.file).type.value);
      type.storage = {layout->storage.size, layout->storage.alignment};
      type.reference_offsets = layout->references;
    }
    layouts[index] = std::move(layout);
    ++completed;
    for (const std::size_t dependent : dependents[index]) {
      if (--pending[dependent] == 0) ready.push(dependent);
    }
  }
  if (completed != mir.files.size()) {
    return fail("inline layout dependency cycle");
  }

  abi.files.reserve(mir.files.size());
  for (const MirFileClass& mir_file : mir.files) {
    const FileSemantics& semantic_file = semantics.file(mir_file.file);
    FileLayout& computed = *layouts.at(mir_file.file.value);
    AbiClassLayout layout = std::move(computed.storage);
    std::optional<AbiTypeDescriptor> type_descriptor;
    if (semantic_file.kind != FileTypeKind::kStruct) {
      type_descriptor =
          lower_type_descriptor(layout, semantics.symbol(mir_file.symbol),
                                std::move(computed.references),
                                mir_file.base_file, semantic_file, semantics);
    }
    AbiFileClass file{mir_file.file,
                      mir_file.symbol,
                      mir_file.base_file,
                      std::move(layout),
                      std::move(type_descriptor),
                      {},
                      {},
                      {},
                      mir_file.member_order};
    file.kind = semantic_file.kind;
    file.static_fields.reserve(mir_file.fields.size());
    for (const MirField& field : mir_file.fields) {
      const SemanticSymbol& symbol = semantics.symbol(field.symbol);
      if (!symbol.is_static) {
        continue;
      }
      file.static_fields.push_back(AbiStaticField{
          field.symbol, symbol.type, lower_linkage(symbol.visibility),
          mangle_abi_static_field(symbol, semantics)});
    }
    file.functions.reserve(mir_file.functions.size());
    for (const MirCallable& function : mir_file.functions) {
      file.functions.push_back(lower_callable(
          function, AbiCallableKind::kFunction, semantic_file, semantics));
    }
    file.constructors.reserve(mir_file.constructors.size());
    for (const MirCallable& constructor : mir_file.constructors) {
      file.constructors.push_back(lower_callable(constructor,
                                                 AbiCallableKind::kConstructor,
                                                 semantic_file, semantics));
    }
    abi.files.push_back(std::move(file));
  }
  return abi;
}

}  // namespace cloth
