// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_SEMA_SEMANTIC_MODEL_H_
#define CLOTH_SEMA_SEMANTIC_MODEL_H_

#include "cloth/ast/ast.h"
#include "cloth/identity/canonical_identity.h"
#include "cloth/sema/visibility.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

enum class IntegerByteOrder {
  kLittleEndian,
  kBigEndian,
};

enum class IntegerMetaOperationKind {
  kRead,
  kWrite,
};

struct IntegerMetaOperation {
  IntegerMetaOperationKind kind;
  IntegerByteOrder byte_order;
  TypeId integer_type;
};

enum class TypeKind {
  kError,
  kBottom,
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
  kErrorClass,
  kFileClass,
  kInterface,
  kArray,
  kNullable,
  kEnum,
  kStruct,
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
  kInterface,
  kEnum,
  kEnumCase,
  kStruct,
  kError,
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
  kPrintEnum,
  kPrintStruct,
};

// Canonical typed scalar value: zero-extended integer/IEEE bits, bool 0/1,
// decoded byte-oriented char, or a nominal enum's declaration-order tag.
struct ScalarConstant {
  TypeId type;
  std::uint64_t bits;

  friend bool operator==(const ScalarConstant&,
                         const ScalarConstant&) = default;
};

struct SwitchLabel {
  ScalarConstant value;
  SourceRange range;
  std::optional<SymbolId> symbol{};
};

struct SwitchSemantics {
  TypeId selector_type;
  std::vector<std::vector<SwitchLabel>> labels;
  bool is_exhaustive{false};
};

// Most symbols cannot throw. Keep the uncommon error set out of the symbol's
// inline storage so large field/local tables do not pay for an empty vector.
class ErrorTypeSet {
 public:
  ErrorTypeSet() = default;
  ErrorTypeSet(const ErrorTypeSet& other) {
    if (other.types_) {
      types_ = std::make_unique<std::vector<TypeId>>(*other.types_);
    }
  }
  ErrorTypeSet& operator=(const ErrorTypeSet& other) {
    if (this == &other) {
      return *this;
    }
    types_ = other.types_ ? std::make_unique<std::vector<TypeId>>(*other.types_)
                          : nullptr;
    return *this;
  }
  ErrorTypeSet(ErrorTypeSet&&) noexcept = default;
  ErrorTypeSet& operator=(ErrorTypeSet&&) noexcept = default;

  ErrorTypeSet& operator=(std::vector<TypeId> types) {
    types_ = types.empty()
                 ? nullptr
                 : std::make_unique<std::vector<TypeId>>(std::move(types));
    return *this;
  }

  [[nodiscard]] bool empty() const noexcept { return values().empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return values().size(); }
  [[nodiscard]] const TypeId& operator[](std::size_t index) const {
    return values()[index];
  }
  [[nodiscard]] auto begin() const noexcept { return values().begin(); }
  [[nodiscard]] auto end() const noexcept { return values().end(); }

  void push_back(TypeId type) {
    if (!types_) {
      types_ = std::make_unique<std::vector<TypeId>>();
    }
    types_->push_back(type);
  }

  [[nodiscard]] std::span<const TypeId> span() const noexcept {
    return values();
  }

  friend bool operator==(const ErrorTypeSet& left, const ErrorTypeSet& right) {
    return left.values() == right.values();
  }
  friend bool operator==(const ErrorTypeSet& left,
                         const std::vector<TypeId>& right) {
    return left.values() == right;
  }

 private:
  [[nodiscard]] const std::vector<TypeId>& values() const noexcept {
    static const std::vector<TypeId> kEmpty;
    return types_ ? *types_ : kEmpty;
  }

  std::unique_ptr<std::vector<TypeId>> types_;
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
  std::optional<std::uint32_t> enum_tag{};
  std::optional<ScalarConstant> static_constant{};
  ErrorTypeSet thrown_types{};
  bool has_explicit_throws{false};
};

enum class ValueCategory {
  kInvalid,
  kValue,
  kMutableLocation,
  kReadOnlyLocation,
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
  std::optional<FileId> interface_dispatch{};
  std::optional<IntegerMetaOperation> integer_meta_operation{};
  bool may_divide_by_zero{false};
};

struct InterfaceImplementation {
  FileId interface_file;
  std::vector<SymbolId> functions;

  friend bool operator==(const InterfaceImplementation&,
                         const InterfaceImplementation&) = default;
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
  FileTypeKind kind{FileTypeKind::kClass};
  std::vector<FileId> direct_interfaces{};
  std::vector<FileId> interfaces{};
  std::vector<SymbolId> interface_functions{};
  std::vector<InterfaceImplementation> interface_implementations{};
  std::optional<std::uint64_t> interface_id{};
  NominalIdentity identity{};
  std::vector<MemberReference> member_order{};
  std::vector<SymbolId> enum_cases{};
  std::map<std::size_t, SwitchSemantics> switches{};
};

class SemanticModel {
 public:
  SemanticModel();

  [[nodiscard]] TypeId error_type() const noexcept;
  [[nodiscard]] TypeId bottom_type() const noexcept;
  [[nodiscard]] TypeId error_root_type() const noexcept;
  [[nodiscard]] TypeId nullable_error_root_type() const noexcept;
  [[nodiscard]] TypeId division_by_zero_type() const noexcept;
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
  friend class ConstantEvaluator;

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
  TypeId bottom_type_{0};
  TypeId error_root_type_{0};
  TypeId nullable_error_root_type_{0};
  TypeId division_by_zero_type_{0};
  TypeId void_type_{0};
  TypeId null_type_{0};
  TypeId bool_type_{0};
  TypeId string_type_{0};
  TypeId object_type_{0};
};

[[nodiscard]] std::string_view type_kind_name(TypeKind kind) noexcept;
[[nodiscard]] std::string_view symbol_kind_name(SymbolKind kind) noexcept;
[[nodiscard]] bool callable_uses_error_abi(SymbolId symbol,
                                           const SemanticModel& semantics);
[[nodiscard]] std::optional<std::uint32_t> enum_constant_tag(
    std::string_view text, TypeId type, const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_SEMA_SEMANTIC_MODEL_H_
