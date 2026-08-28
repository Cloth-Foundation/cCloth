#include "cloth/sema/semantic_model.h"

#include <array>
#include <utility>

namespace cloth {

SemanticModel::SemanticModel() {
  error_type_ = add_type(SemanticType{TypeKind::kError, "<error>", {}});
  void_type_ = add_type(SemanticType{TypeKind::kVoid, "void", {}});
  null_type_ = add_type(SemanticType{TypeKind::kNull, "null", {}});
  bool_type_ = add_type(SemanticType{TypeKind::kBool, "bool", {}});
  const TypeId char_type = add_type(SemanticType{TypeKind::kChar, "char", {}});
  const TypeId byte_type = add_type(SemanticType{TypeKind::kByte, "byte", {}});
  const TypeId int8 = add_type(SemanticType{TypeKind::kInt8, "int8", {}});
  const TypeId int16 = add_type(SemanticType{TypeKind::kInt16, "int16", {}});
  const TypeId int32 = add_type(SemanticType{TypeKind::kInt32, "int32", {}});
  const TypeId int64 = add_type(SemanticType{TypeKind::kInt64, "int64", {}});
  const TypeId uint8 = add_type(SemanticType{TypeKind::kUint8, "uint8", {}});
  const TypeId uint16 = add_type(SemanticType{TypeKind::kUint16, "uint16", {}});
  const TypeId uint32 = add_type(SemanticType{TypeKind::kUint32, "uint32", {}});
  const TypeId uint64 = add_type(SemanticType{TypeKind::kUint64, "uint64", {}});
  const TypeId float32 =
      add_type(SemanticType{TypeKind::kFloat32, "float32", {}});
  const TypeId float64 =
      add_type(SemanticType{TypeKind::kFloat64, "float64", {}});
  string_type_ = add_type(SemanticType{TypeKind::kString, "string", {}});
  object_type_ = add_type(SemanticType{TypeKind::kObject, "object", {}});
  add_type_alias("int", int32);
  add_type_alias("uint", uint32);
  add_type_alias("float", float32);

  const std::array primitive_prints{
      std::pair{string_type_, IntrinsicKind::kPrintString},
      std::pair{bool_type_, IntrinsicKind::kPrintBool},
      std::pair{char_type, IntrinsicKind::kPrintChar},
      std::pair{byte_type, IntrinsicKind::kPrintUint8},
      std::pair{int8, IntrinsicKind::kPrintInt8},
      std::pair{int16, IntrinsicKind::kPrintInt16},
      std::pair{int32, IntrinsicKind::kPrintInt32},
      std::pair{int64, IntrinsicKind::kPrintInt64},
      std::pair{uint8, IntrinsicKind::kPrintUint8},
      std::pair{uint16, IntrinsicKind::kPrintUint16},
      std::pair{uint32, IntrinsicKind::kPrintUint32},
      std::pair{uint64, IntrinsicKind::kPrintUint64},
      std::pair{float32, IntrinsicKind::kPrintFloat32},
      std::pair{float64, IntrinsicKind::kPrintFloat64},
      std::pair{null_type_, IntrinsicKind::kPrintObject},
      std::pair{object_type_, IntrinsicKind::kPrintObject},
  };
  for (const auto& [type, intrinsic] : primitive_prints) {
    add_intrinsic("print", {type}, intrinsic);
    add_intrinsic("println", {type}, intrinsic);
  }
  add_intrinsic("println", {}, IntrinsicKind::kPrintNewline);
}

TypeId SemanticModel::error_type() const noexcept { return error_type_; }

TypeId SemanticModel::void_type() const noexcept { return void_type_; }

TypeId SemanticModel::null_type() const noexcept { return null_type_; }

TypeId SemanticModel::bool_type() const noexcept { return bool_type_; }

TypeId SemanticModel::string_type() const noexcept { return string_type_; }

TypeId SemanticModel::object_type() const noexcept { return object_type_; }

TypeId SemanticModel::add_type(SemanticType type) {
  const TypeId id{types_.size()};
  type_names_.push_back(TypeName{type.name, id});
  types_.push_back(std::move(type));
  return id;
}

TypeId SemanticModel::get_array_type(TypeId element_type) {
  for (std::size_t index = 0; index < types_.size(); ++index) {
    const SemanticType& type = types_[index];
    if (type.kind == TypeKind::kArray && type.element_type == element_type) {
      return TypeId{index};
    }
  }
  return add_type(SemanticType{TypeKind::kArray,
                               types_.at(element_type.value).name + "[]",
                               {},
                               element_type});
}

TypeId SemanticModel::get_nullable_type(TypeId underlying_type) {
  for (std::size_t index = 0; index < types_.size(); ++index) {
    const SemanticType& type = types_[index];
    if (type.kind == TypeKind::kNullable &&
        type.element_type == underlying_type) {
      return TypeId{index};
    }
  }
  return add_type(SemanticType{TypeKind::kNullable,
                               types_.at(underlying_type.value).name + "?",
                               {},
                               underlying_type});
}

void SemanticModel::add_type_alias(std::string name, TypeId type) {
  type_names_.push_back(TypeName{std::move(name), type});
}

void SemanticModel::add_intrinsic(std::string name,
                                  std::vector<TypeId> parameter_types,
                                  IntrinsicKind intrinsic) {
  const SourceLocation core_location{"<core>"};
  static_cast<void>(add_symbol(SemanticSymbol{
      SymbolKind::kFunction,
      std::move(name),
      void_type_,
      std::move(parameter_types),
      Visibility::kPublic,
      std::nullopt,
      point_range(core_location),
      true,
      {},
      intrinsic,
  }));
}

SymbolId SemanticModel::add_symbol(SemanticSymbol symbol) {
  const SymbolId id{symbols_.size()};
  symbols_.push_back(std::move(symbol));
  return id;
}

FileId SemanticModel::add_file(FileSemantics file) {
  const FileId id{files_.size()};
  files_.push_back(std::move(file));
  return id;
}

std::optional<TypeId> SemanticModel::find_type(
    std::string_view name) const noexcept {
  for (const TypeName& entry : type_names_) {
    if (entry.name == name) {
      return entry.type;
    }
  }
  return std::nullopt;
}

std::vector<SymbolId> SemanticModel::find_intrinsics(
    std::string_view name) const {
  std::vector<SymbolId> matches;
  for (std::size_t index = 0; index < symbols_.size(); ++index) {
    const SemanticSymbol& symbol = symbols_[index];
    if (symbol.intrinsic != IntrinsicKind::kNone && symbol.name == name) {
      matches.push_back(SymbolId{index});
    }
  }
  return matches;
}

const SemanticType& SemanticModel::type(TypeId id) const {
  return types_.at(id.value);
}

const SemanticSymbol& SemanticModel::symbol(SymbolId id) const {
  return symbols_.at(id.value);
}

const FileSemantics& SemanticModel::file(FileId id) const {
  return files_.at(id.value);
}

FileSemantics& SemanticModel::mutable_file(FileId id) {
  return files_.at(id.value);
}

SemanticSymbol& SemanticModel::mutable_symbol(SymbolId id) {
  return symbols_.at(id.value);
}

std::span<const SemanticType> SemanticModel::types() const noexcept {
  return types_;
}

std::span<const SemanticSymbol> SemanticModel::symbols() const noexcept {
  return symbols_;
}

std::span<const FileSemantics> SemanticModel::files() const noexcept {
  return files_;
}

std::string_view type_kind_name(TypeKind kind) noexcept {
  switch (kind) {
    case TypeKind::kError:
      return "error";
    case TypeKind::kVoid:
      return "void";
    case TypeKind::kNull:
      return "null";
    case TypeKind::kBool:
      return "bool";
    case TypeKind::kChar:
      return "char";
    case TypeKind::kByte:
      return "byte";
    case TypeKind::kInt8:
      return "int8";
    case TypeKind::kInt16:
      return "int16";
    case TypeKind::kInt32:
      return "int32";
    case TypeKind::kInt64:
      return "int64";
    case TypeKind::kUint8:
      return "uint8";
    case TypeKind::kUint16:
      return "uint16";
    case TypeKind::kUint32:
      return "uint32";
    case TypeKind::kUint64:
      return "uint64";
    case TypeKind::kFloat32:
      return "float32";
    case TypeKind::kFloat64:
      return "float64";
    case TypeKind::kString:
      return "string";
    case TypeKind::kObject:
      return "object";
    case TypeKind::kFileClass:
      return "file class";
    case TypeKind::kArray:
      return "array";
    case TypeKind::kNullable:
      return "nullable reference";
  }
  return "unknown";
}

std::string_view symbol_kind_name(SymbolKind kind) noexcept {
  switch (kind) {
    case SymbolKind::kFileClass:
      return "file class";
    case SymbolKind::kField:
      return "field";
    case SymbolKind::kFunction:
      return "function";
    case SymbolKind::kConstructor:
      return "constructor";
    case SymbolKind::kParameter:
      return "parameter";
    case SymbolKind::kLocal:
      return "local";
    case SymbolKind::kSelf:
      return "self";
  }
  return "unknown";
}

}  // namespace cloth
