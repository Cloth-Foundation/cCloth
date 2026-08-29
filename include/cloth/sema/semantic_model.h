#ifndef CLOTH_SEMA_SEMANTIC_MODEL_H_
#define CLOTH_SEMA_SEMANTIC_MODEL_H_

#include "cloth/ast/ast.h"
#include "cloth/sema/visibility.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cloth {

class SemanticAnalyzer;

struct FileId {
  std::size_t value;

  friend bool operator==(const FileId&, const FileId&) = default;
};

struct TypeId {
  std::size_t value;

  friend bool operator==(const TypeId&, const TypeId&) = default;
};

struct SymbolId {
  std::size_t value;

  friend bool operator==(const SymbolId&, const SymbolId&) = default;
};

enum class TypeKind {
  kError,
  kVoid,
  kNull,
  kBool,
  kChar,
  kByte,
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kUint8,
  kUint16,
  kUint32,
  kUint64,
  kFloat32,
  kFloat64,
  kString,
  kObject,
  kFileClass,
  kArray,
  kNullable,
};

struct SemanticType {
  TypeKind kind;
  std::string name;
  std::optional<FileId> file;
  std::optional<TypeId> element_type{};
};

enum class SymbolKind {
  kFileClass,
  kField,
  kFunction,
  kConstructor,
  kParameter,
  kLocal,
  kSelf,
};

enum class IntrinsicKind {
  kNone,
  kPrintString,
  kPrintBool,
  kPrintChar,
  kPrintInt8,
  kPrintInt16,
  kPrintInt32,
  kPrintInt64,
  kPrintUint8,
  kPrintUint16,
  kPrintUint32,
  kPrintUint64,
  kPrintFloat32,
  kPrintFloat64,
  kPrintObject,
  kPrintNewline,
};

struct SemanticSymbol {
  SymbolKind kind;
  std::string name;
  TypeId type;
  std::vector<TypeId> parameter_types;
  Visibility visibility;
  std::optional<FileId> file;
  SourceRange range;
  bool is_valid{true};
  std::vector<SymbolId> parameter_symbols{};
  std::optional<SymbolId> base_constructor{};
  IntrinsicKind intrinsic{IntrinsicKind::kNone};
  bool is_final{false};
  bool is_static{false};
  bool is_override{false};
  bool is_abstract{false};
  std::optional<std::size_t> virtual_slot{};
  std::optional<SymbolId> overridden_symbol{};
};

enum class ValueCategory {
  kInvalid,
  kValue,
  kMutableLocation,
  kCallable,
  kType,
  kSuper,
};

struct ExpressionSemantics {
  TypeId type{0};
  ValueCategory category{ValueCategory::kInvalid};
  std::optional<SymbolId> symbol{};
  bool is_presence_test{false};
  std::optional<TypeId> checked_type{};
  bool is_base_qualified{false};
};

struct FileSemantics {
  TypeId type;
  SymbolId symbol;
  SymbolId self_symbol;
  std::vector<SymbolId> fields;
  std::vector<SymbolId> functions;
  std::vector<SymbolId> constructors;
  std::vector<ExpressionSemantics> expressions;
  std::vector<std::optional<SymbolId>> statement_symbols;
  bool is_valid{true};
  std::optional<FileId> base_file{};
  std::vector<SymbolId> virtual_functions{};
  std::vector<SymbolId> abstract_functions{};
  bool is_abstract{false};
  bool is_sealed{false};
};

class SemanticModel {
 public:
  SemanticModel();

  [[nodiscard]] TypeId error_type() const noexcept;
  [[nodiscard]] TypeId void_type() const noexcept;
  [[nodiscard]] TypeId null_type() const noexcept;
  [[nodiscard]] TypeId bool_type() const noexcept;
  [[nodiscard]] TypeId string_type() const noexcept;
  [[nodiscard]] TypeId object_type() const noexcept;

  [[nodiscard]] std::optional<TypeId> find_type(
      std::string_view name) const noexcept;
  [[nodiscard]] std::vector<SymbolId> find_intrinsics(
      std::string_view name) const;
  [[nodiscard]] const SemanticType& type(TypeId id) const;
  [[nodiscard]] const SemanticSymbol& symbol(SymbolId id) const;
  [[nodiscard]] const FileSemantics& file(FileId id) const;

  [[nodiscard]] std::span<const SemanticType> types() const noexcept;
  [[nodiscard]] std::span<const SemanticSymbol> symbols() const noexcept;
  [[nodiscard]] std::span<const FileSemantics> files() const noexcept;

 private:
  friend class SemanticAnalyzer;

  struct TypeName {
    std::string name;
    TypeId type;
  };

  [[nodiscard]] TypeId add_type(SemanticType type);
  [[nodiscard]] TypeId get_array_type(TypeId element_type);
  [[nodiscard]] TypeId get_nullable_type(TypeId underlying_type);
  void add_type_alias(std::string name, TypeId type);
  void add_intrinsic(std::string name, std::vector<TypeId> parameter_types,
                     IntrinsicKind intrinsic);
  [[nodiscard]] SymbolId add_symbol(SemanticSymbol symbol);
  [[nodiscard]] FileId add_file(FileSemantics file);
  [[nodiscard]] FileSemantics& mutable_file(FileId id);
  [[nodiscard]] SemanticSymbol& mutable_symbol(SymbolId id);

  std::vector<SemanticType> types_;
  std::vector<TypeName> type_names_;
  std::vector<SemanticSymbol> symbols_;
  std::vector<FileSemantics> files_;
  TypeId error_type_{0};
  TypeId void_type_{0};
  TypeId null_type_{0};
  TypeId bool_type_{0};
  TypeId string_type_{0};
  TypeId object_type_{0};
};

[[nodiscard]] std::string_view type_kind_name(TypeKind kind) noexcept;
[[nodiscard]] std::string_view symbol_kind_name(SymbolKind kind) noexcept;

}  // namespace cloth

#endif  // CLOTH_SEMA_SEMANTIC_MODEL_H_
