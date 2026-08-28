#include "cloth/abi/abi.h"

#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/sema/visibility.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloth {
namespace {

std::uint64_t add_size(std::uint64_t left, std::uint64_t right) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

std::uint64_t multiply_size(std::uint64_t left, std::uint64_t right) noexcept {
  if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left * right;
}

std::uint32_t pointer_bit_width(const TargetDataLayout& target) noexcept {
  const std::uint64_t bits = multiply_size(target.pointer.size, 8);
  if (bits > std::numeric_limits<std::uint32_t>::max()) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(bits);
}

AbiTypeLayout make_type_layout(TypeId type, AbiTypeKind kind,
                               std::uint32_t bit_width, std::uint64_t size,
                               std::uint64_t alignment) {
  return AbiTypeLayout{type, kind, bit_width, SizeAlignment{size, alignment}};
}

AbiTypeLayout lower_type(TypeId type, const SemanticType& semantic_type,
                         const TargetDataLayout& target) {
  switch (semantic_type.kind) {
    case TypeKind::kError:
      return make_type_layout(type, AbiTypeKind::kInvalid, 0, 0, 1);
    case TypeKind::kVoid:
      return make_type_layout(type, AbiTypeKind::kVoid, 0, 0, 1);
    case TypeKind::kNull:
    case TypeKind::kString:
    case TypeKind::kFileClass:
    case TypeKind::kArray:
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

std::string encode_name(std::string_view name) {
  return std::to_string(name.size()) + "_" + std::string{name};
}

std::string encode_type(TypeId type, const SemanticModel& semantics) {
  const SemanticType& semantic_type = semantics.type(type);
  switch (semantic_type.kind) {
    case TypeKind::kError:
      return "e";
    case TypeKind::kVoid:
      return "v";
    case TypeKind::kNull:
      return "n";
    case TypeKind::kBool:
      return "b";
    case TypeKind::kChar:
      return "c";
    case TypeKind::kByte:
      return "y";
    case TypeKind::kInt8:
      return "i8";
    case TypeKind::kInt16:
      return "i16";
    case TypeKind::kInt32:
      return "i32";
    case TypeKind::kInt64:
      return "i64";
    case TypeKind::kUint8:
      return "u8";
    case TypeKind::kUint16:
      return "u16";
    case TypeKind::kUint32:
      return "u32";
    case TypeKind::kUint64:
      return "u64";
    case TypeKind::kFloat32:
      return "f32";
    case TypeKind::kFloat64:
      return "f64";
    case TypeKind::kString:
      return "s";
    case TypeKind::kFileClass:
      return "r" + encode_name(semantic_type.name);
    case TypeKind::kArray:
      return semantic_type.element_type
                 ? "a" + encode_type(*semantic_type.element_type, semantics)
                 : "ae";
  }
  return "e";
}

AbiLinkage lower_linkage(Visibility visibility) noexcept {
  return visibility == Visibility::kPublic ? AbiLinkage::kExternal
                                           : AbiLinkage::kInternal;
}

AbiClassLayout lower_class_layout(const MirFileClass& file,
                                  const SemanticModel& semantics,
                                  const std::vector<AbiTypeLayout>& types,
                                  const TargetDataLayout& target) {
  const std::uint64_t header_size =
      multiply_size(target.pointer.size, target.object_header_words);
  std::uint64_t offset = header_size;
  std::uint64_t class_alignment =
      target.pointer.alignment == 0 ? 1 : target.pointer.alignment;
  std::vector<AbiFieldLayout> fields;
  fields.reserve(file.fields.size());
  for (const MirField& field : file.fields) {
    const SemanticSymbol& symbol = semantics.symbol(field.symbol);
    if (symbol.is_static) {
      continue;
    }
    const TypeId type = symbol.type;
    const AbiTypeLayout& type_layout = types.at(type.value);
    class_alignment = std::max(class_alignment, type_layout.storage.alignment);
    offset = align_to(offset, type_layout.storage.alignment);
    fields.push_back(AbiFieldLayout{field.symbol, type, offset});
    offset = add_size(offset, type_layout.storage.size);
  }
  return AbiClassLayout{header_size, align_to(offset, class_alignment),
                        class_alignment, std::move(fields)};
}

AbiCallable lower_callable(const MirCallable& callable, AbiCallableKind kind,
                           const FileSemantics& file,
                           const SemanticModel& semantics) {
  const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
  std::vector<AbiParameter> parameters;
  parameters.reserve(
      callable.parameters.size() +
      (kind == AbiCallableKind::kFunction && !symbol.is_static ? 1U : 0U));
  if (kind == AbiCallableKind::kFunction && !symbol.is_static) {
    parameters.push_back(AbiParameter{AbiParameterKind::kReceiver,
                                      file.self_symbol,
                                      semantics.symbol(file.symbol).type});
  }
  for (const SymbolId parameter : callable.parameters) {
    parameters.push_back(AbiParameter{AbiParameterKind::kExplicit, parameter,
                                      semantics.symbol(parameter).type});
  }
  const TypeId return_type = kind == AbiCallableKind::kConstructor
                                 ? semantics.symbol(file.symbol).type
                                 : symbol.type;
  return AbiCallable{callable.symbol,
                     kind,
                     lower_linkage(symbol.visibility),
                     AbiCallingConvention::kC,
                     mangle_abi_symbol(symbol, semantics),
                     return_type,
                     std::move(parameters)};
}

}  // namespace

std::string mangle_abi_symbol(const SemanticSymbol& symbol,
                              const SemanticModel& semantics) {
  std::string result = "_C1";
  result += symbol.kind == SymbolKind::kConstructor ? 'C' : 'F';
  if (symbol.file) {
    const FileSemantics& file = semantics.file(*symbol.file);
    result += encode_name(semantics.symbol(file.symbol).name);
  } else {
    result += encode_name("invalid");
  }
  result += encode_name(symbol.name);
  result += 'P';
  result += std::to_string(symbol.parameter_types.size());
  for (const TypeId parameter : symbol.parameter_types) {
    result += '_';
    result += encode_type(parameter, semantics);
  }
  return result;
}

std::string mangle_abi_static_field(const SemanticSymbol& symbol,
                                    const SemanticModel& semantics) {
  std::string result = "_C1S";
  if (symbol.file) {
    const FileSemantics& file = semantics.file(*symbol.file);
    result += encode_name(semantics.symbol(file.symbol).name);
  } else {
    result += encode_name("invalid");
  }
  result += encode_name(symbol.name);
  return result;
}

AbiModule lower_to_abi(const MirModule& mir, const SemanticModel& semantics,
                       TargetDataLayout target) {
  AbiModule abi{std::move(target), {}, {}};
  abi.types.reserve(semantics.types().size());
  for (std::size_t index = 0; index < semantics.types().size(); ++index) {
    const TypeId type{index};
    abi.types.push_back(lower_type(type, semantics.type(type), abi.target));
  }

  abi.files.reserve(mir.files.size());
  for (const MirFileClass& mir_file : mir.files) {
    const FileSemantics& semantic_file = semantics.file(mir_file.file);
    AbiFileClass file{
        mir_file.file,
        mir_file.symbol,
        lower_class_layout(mir_file, semantics, abi.types, abi.target),
        {},
        {},
        {},
        mir_file.member_order};
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
