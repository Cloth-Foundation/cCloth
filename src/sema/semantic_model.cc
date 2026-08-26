#include "cloth/sema/semantic_model.h"

#include <utility>

namespace cloth {

SemanticModel::SemanticModel() {
  error_type_ = add_type(SemanticType{TypeKind::kError, "<error>", {}});
  no_value_type_ = add_type(SemanticType{TypeKind::kNoValue, "<no-value>", {}});
  null_type_ = add_type(SemanticType{TypeKind::kNull, "null", {}});
  bool_type_ = add_type(SemanticType{TypeKind::kBool, "bool", {}});
  static_cast<void>(add_type(SemanticType{TypeKind::kChar, "char", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kByte, "byte", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kInt8, "int8", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kInt16, "int16", {}}));
  const TypeId int32 = add_type(SemanticType{TypeKind::kInt32, "int32", {}});
  static_cast<void>(add_type(SemanticType{TypeKind::kInt64, "int64", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kUint8, "uint8", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kUint16, "uint16", {}}));
  const TypeId uint32 = add_type(SemanticType{TypeKind::kUint32, "uint32", {}});
  static_cast<void>(add_type(SemanticType{TypeKind::kUint64, "uint64", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kFloat32, "float32", {}}));
  static_cast<void>(add_type(SemanticType{TypeKind::kFloat64, "float64", {}}));
  string_type_ = add_type(SemanticType{TypeKind::kString, "String", {}});
  add_type_alias("int", int32);
  add_type_alias("uint", uint32);

  const SourceLocation core_location{"<core>"};
  static_cast<void>(add_symbol(SemanticSymbol{
      SymbolKind::kFunction,
      "print",
      no_value_type_,
      {string_type_},
      Visibility::kPublic,
      std::nullopt,
      point_range(core_location),
      true,
      {},
      IntrinsicKind::kPrintString,
  }));
  static_cast<void>(add_symbol(SemanticSymbol{
      SymbolKind::kFunction,
      "print",
      no_value_type_,
      {int32},
      Visibility::kPublic,
      std::nullopt,
      point_range(core_location),
      true,
      {},
      IntrinsicKind::kPrintInt32,
  }));
  static_cast<void>(add_symbol(SemanticSymbol{
      SymbolKind::kFunction,
      "print",
      no_value_type_,
      {bool_type_},
      Visibility::kPublic,
      std::nullopt,
      point_range(core_location),
      true,
      {},
      IntrinsicKind::kPrintBool,
  }));
}

TypeId SemanticModel::error_type() const noexcept { return error_type_; }

TypeId SemanticModel::no_value_type() const noexcept { return no_value_type_; }

TypeId SemanticModel::null_type() const noexcept { return null_type_; }

TypeId SemanticModel::bool_type() const noexcept { return bool_type_; }

TypeId SemanticModel::string_type() const noexcept { return string_type_; }

TypeId SemanticModel::add_type(SemanticType type) {
  const TypeId id{types_.size()};
  type_names_.push_back(TypeName{type.name, id});
  types_.push_back(std::move(type));
  return id;
}

void SemanticModel::add_type_alias(std::string name, TypeId type) {
  type_names_.push_back(TypeName{std::move(name), type});
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
    case TypeKind::kNoValue:
      return "no-value";
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
      return "String";
    case TypeKind::kFileClass:
      return "file class";
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
