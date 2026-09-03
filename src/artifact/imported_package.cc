// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"

#include "cloth/abi/aggregate_limits.h"
#include "cloth/identity/package_identity.h"
#include "cloth/sema/canonical_identity.h"
#include "cloth/sema/numeric_types.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloth {
namespace {

class IssueCollector {
 public:
  void add(
      std::string record, std::string message,
      ImportedPackageIssueCode code = ImportedPackageIssueCode::kInvalidModel) {
    issues_.push_back(
        ImportedPackageIssue{std::move(record), std::move(message), code});
  }

  [[nodiscard]] std::vector<ImportedPackageIssue> take() {
    return std::move(issues_);
  }

 private:
  std::vector<ImportedPackageIssue> issues_;
};

bool is_nominal(TypeKind kind) {
  return kind == TypeKind::kFileClass || kind == TypeKind::kInterface ||
         kind == TypeKind::kEnum || kind == TypeKind::kStruct;
}

bool is_structural(TypeKind kind) {
  return kind == TypeKind::kArray || kind == TypeKind::kNullable;
}

bool valid_source_package(std::string_view source_package) {
  if (source_package.empty()) {
    return true;
  }
  std::size_t begin = 0;
  while (begin < source_package.size()) {
    const std::size_t end = source_package.find('.', begin);
    const std::string_view component = source_package.substr(
        begin, end == std::string_view::npos ? source_package.size() - begin
                                             : end - begin);
    if (!is_valid_identifier(component)) {
      return false;
    }
    if (end == std::string_view::npos) {
      return true;
    }
    begin = end + 1;
  }
  return false;
}

bool valid_nominal_identity(const NominalIdentity& identity) {
  return is_valid_package_name(identity.package.name) &&
         is_valid_package_version(identity.package.version) &&
         valid_source_package(identity.source_package) &&
         is_valid_identifier(identity.name);
}

std::string logical_path(const NominalIdentity& identity) {
  std::string result;
  result.reserve(identity.source_package.size() + identity.name.size() + 4);
  for (const char character : identity.source_package) {
    result.push_back(character == '.' ? '/' : character);
  }
  if (!result.empty()) {
    result.push_back('/');
  }
  result += identity.name;
  result += ".co";
  return result;
}

std::string nominal_display_name(const NominalIdentity& identity) {
  std::string result = identity.package.name;
  if (!result.empty() && !identity.source_package.empty())
    result.push_back('.');
  result += identity.source_package;
  if (!result.empty()) result.push_back('.');
  result += identity.name;
  return result;
}

ImportedSourceLocation import_location(const SemanticSymbol& symbol,
                                       std::string_view path) {
  return ImportedSourceLocation{std::string{path}, symbol.range.begin.line,
                                symbol.range.begin.column};
}

CanonicalMemberKind canonical_member_kind(const SemanticSymbol& symbol) {
  switch (symbol.kind) {
    case SymbolKind::kField:
      return symbol.is_static ? CanonicalMemberKind::kStaticField
                              : CanonicalMemberKind::kInstanceField;
    case SymbolKind::kConstructor:
      return CanonicalMemberKind::kConstructor;
    case SymbolKind::kFunction:
      return CanonicalMemberKind::kFunction;
    case SymbolKind::kFileClass:
    case SymbolKind::kParameter:
    case SymbolKind::kLocal:
    case SymbolKind::kSelf:
    case SymbolKind::kInterface:
    case SymbolKind::kEnum:
    case SymbolKind::kEnumCase:
    case SymbolKind::kStruct:
      break;
  }
  return CanonicalMemberKind::kFunction;
}

ImportedMemberKind imported_member_kind(SymbolKind kind) {
  switch (kind) {
    case SymbolKind::kField:
      return ImportedMemberKind::kField;
    case SymbolKind::kConstructor:
      return ImportedMemberKind::kConstructor;
    case SymbolKind::kFunction:
      return ImportedMemberKind::kFunction;
    case SymbolKind::kFileClass:
    case SymbolKind::kParameter:
    case SymbolKind::kLocal:
    case SymbolKind::kSelf:
    case SymbolKind::kInterface:
    case SymbolKind::kEnum:
    case SymbolKind::kEnumCase:
    case SymbolKind::kStruct:
      break;
  }
  return ImportedMemberKind::kFunction;
}

std::optional<ImportedLiteral> returned_literal(const MirBody& body) {
  for (const MirBasicBlock& block : body.blocks) {
    const auto* returned =
        std::get_if<MirReturnTerminator>(&block.terminator.data);
    if (returned == nullptr || !returned->value) {
      continue;
    }
    for (const MirBasicBlock& candidate : body.blocks) {
      for (const MirInstruction& instruction : candidate.instructions) {
        if (instruction.result != returned->value) {
          continue;
        }
        if (const auto* literal =
                std::get_if<MirLiteralInstruction>(&instruction.data)) {
          return ImportedLiteral{literal->kind, literal->lexeme};
        }
      }
    }
  }
  return std::nullopt;
}

const AbiFileClass* find_abi_file(const AbiModule& abi, FileId id) {
  const auto file = std::ranges::find(abi.files, id, &AbiFileClass::file);
  return file == abi.files.end() ? nullptr : &*file;
}

const MirFileClass* find_mir_file(const MirModule& mir, FileId id) {
  const auto file = std::ranges::find(mir.files, id, &MirFileClass::file);
  return file == mir.files.end() ? nullptr : &*file;
}

const MirField* find_mir_field(const MirFileClass& file, SymbolId id) {
  const auto field = std::ranges::find(file.fields, id, &MirField::symbol);
  return field == file.fields.end() ? nullptr : &*field;
}

std::string member_identity(SymbolId id, const SemanticModel& semantics) {
  const SemanticSymbol& symbol = semantics.symbol(id);
  return canonical_symbol_identity(symbol, semantics,
                                   canonical_member_kind(symbol));
}

std::string file_identity(FileId id, const SemanticModel& semantics) {
  return canonical_nominal_identity(semantics.file(id).identity);
}

std::vector<std::string> symbol_identities(const std::vector<SymbolId>& symbols,
                                           const SemanticModel& semantics) {
  std::vector<std::string> result;
  result.reserve(symbols.size());
  for (const SymbolId symbol : symbols) {
    result.push_back(member_identity(symbol, semantics));
  }
  return result;
}

std::vector<std::string> file_identities(const std::vector<FileId>& files,
                                         const SemanticModel& semantics) {
  std::vector<std::string> result;
  result.reserve(files.size());
  for (const FileId file : files) {
    result.push_back(file_identity(file, semantics));
  }
  return result;
}

std::vector<std::string> declaration_order(const FileSemantics& file,
                                           const AbiFileClass& abi_file,
                                           const SemanticModel& semantics) {
  std::vector<std::string> result;
  result.reserve(abi_file.member_order.size());
  for (const MemberReference& member : abi_file.member_order) {
    switch (member.kind) {
      case DeclarationKind::kField:
        result.push_back(
            member_identity(file.fields.at(member.index), semantics));
        break;
      case DeclarationKind::kFunction:
        result.push_back(
            member_identity(file.functions.at(member.index), semantics));
        break;
      case DeclarationKind::kConstructor:
        result.push_back(
            member_identity(file.constructors.at(member.index), semantics));
        break;
      case DeclarationKind::kNestedType:
        break;
    }
  }
  return result;
}

ImportedMember import_member(SymbolId id, std::string_view owner,
                             std::string_view path,
                             const SemanticModel& semantics,
                             const MirFileClass& mir_file) {
  const SemanticSymbol& symbol = semantics.symbol(id);
  std::vector<ImportedParameter> parameters;
  parameters.reserve(symbol.parameter_symbols.size());
  for (const SymbolId parameter_id : symbol.parameter_symbols) {
    const SemanticSymbol& parameter = semantics.symbol(parameter_id);
    parameters.push_back(ImportedParameter{
        parameter.name, canonical_type_identity(parameter.type, semantics),
        parameter.is_final});
  }

  std::optional<ImportedLiteral> static_value;
  if (symbol.kind == SymbolKind::kField && symbol.is_static) {
    const MirField* field = find_mir_field(mir_file, id);
    if (field != nullptr && field->initializer) {
      static_value = returned_literal(*field->initializer);
      if (static_value && symbol.static_constant) {
        const auto constant = *symbol.static_constant;
        const TypeKind kind = semantics.type(constant.type).kind;
        if (kind == TypeKind::kEnum) {
          static_value = ImportedLiteral{LiteralKind::kEnum,
                                         std::to_string(constant.bits)};
        } else if (is_valid_integer_bits(constant.bits, kind)) {
          const auto properties = *numeric_type_properties(kind);
          const bool negative =
              properties.category == NumericCategory::kSignedInteger &&
              (constant.bits &
               (std::uint64_t{1} << (properties.bit_width - 1))) != 0;
          const std::uint64_t magnitude =
              !negative ? constant.bits
              : properties.bit_width == 64
                  ? std::uint64_t{0} - constant.bits
                  : (std::uint64_t{1} << properties.bit_width) - constant.bits;
          static_value = ImportedLiteral{
              LiteralKind::kInteger,
              std::string{negative ? "-" : ""} + std::to_string(magnitude)};
        }
      }
    }
  }

  return ImportedMember{
      member_identity(id, semantics),
      std::string{owner},
      symbol.name,
      imported_member_kind(symbol.kind),
      symbol.visibility,
      canonical_type_identity(symbol.type, semantics),
      std::move(parameters),
      import_location(symbol, path),
      symbol.is_final,
      symbol.is_static,
      symbol.is_override,
      symbol.is_abstract,
      symbol.virtual_slot,
      symbol.overridden_symbol ? std::optional<std::string>{member_identity(
                                     *symbol.overridden_symbol, semantics)}
                               : std::nullopt,
      symbol.base_constructor ? std::optional<std::string>{member_identity(
                                    *symbol.base_constructor, semantics)}
                              : std::nullopt,
      std::move(static_value)};
}

ImportedCallableAbi import_callable(const AbiCallable& callable,
                                    const SemanticModel& semantics) {
  const SemanticSymbol& symbol = semantics.symbol(callable.symbol);
  std::vector<ImportedAbiParameter> parameters;
  parameters.reserve(callable.parameters.size());
  for (const AbiParameter& parameter : callable.parameters) {
    parameters.push_back(ImportedAbiParameter{
        parameter.kind, canonical_type_identity(parameter.type, semantics),
        parameter.passing});
  }
  std::optional<std::string> initializer_identity;
  std::optional<std::string> initializer_return_type;
  std::vector<ImportedAbiParameter> initializer_parameters;
  const bool class_constructor =
      callable.kind == AbiCallableKind::kConstructor &&
      semantics.file(*symbol.file).kind == FileTypeKind::kClass;
  if (class_constructor) {
    initializer_identity = canonical_symbol_identity(
        symbol, semantics, CanonicalMemberKind::kConstructorInitializer);
    initializer_return_type =
        canonical_type_identity(semantics.void_type(), semantics);
    initializer_parameters.reserve(parameters.size() + 1);
    initializer_parameters.push_back(ImportedAbiParameter{
        AbiParameterKind::kReceiver, file_identity(*symbol.file, semantics)});
    initializer_parameters.insert(initializer_parameters.end(),
                                  parameters.begin(), parameters.end());
  }
  return ImportedCallableAbi{
      member_identity(callable.symbol, semantics),
      callable.kind,
      callable.linkage,
      callable.calling_convention,
      callable.mangled_name,
      std::move(initializer_identity),
      callable.initializer_mangled_name,
      callable.initializer_linkage,
      std::move(initializer_return_type),
      std::move(initializer_parameters),
      canonical_type_identity(callable.return_type, semantics),
      std::move(parameters),
      callable.return_mode,
      callable.receiver_mode,
      AbiReturnMode::kVoid,
      class_constructor ? AbiReceiverMode::kReference : AbiReceiverMode::kNone};
}

ImportedClassAbi import_class_abi(const AbiFileClass& file,
                                  const SemanticModel& semantics) {
  const FileSemantics& semantic_file = semantics.file(file.file);
  std::vector<ImportedFieldLayout> fields;
  fields.reserve(file.layout.fields.size());
  for (const AbiFieldLayout& field : file.layout.fields) {
    fields.push_back(ImportedFieldLayout{
        member_identity(field.symbol, semantics),
        canonical_type_identity(field.type, semantics), field.offset});
  }

  std::optional<ImportedTypeDescriptor> descriptor;
  if (file.type_descriptor) {
    std::vector<ImportedInterfaceDispatch> interfaces;
    interfaces.reserve(file.type_descriptor->interfaces.size());
    for (const AbiTypeDescriptor::InterfaceDispatch& interface_dispatch :
         file.type_descriptor->interfaces) {
      interfaces.push_back(ImportedInterfaceDispatch{
          file_identity(interface_dispatch.interface_file, semantics),
          interface_dispatch.interface_id,
          symbol_identities(interface_dispatch.functions, semantics)});
    }

    descriptor = ImportedTypeDescriptor{
        file.type_descriptor->kind,
        canonical_member_identity(semantic_file.identity,
                                  CanonicalMemberKind::kDescriptor, ""),
        file.type_descriptor->parent_file
            ? std::optional<std::string>{file_identity(
                  *file.type_descriptor->parent_file, semantics)}
            : std::nullopt,
        file.type_descriptor->name,
        file.type_descriptor->size,
        file.type_descriptor->alignment,
        file.type_descriptor->reference_offsets,
        symbol_identities(file.type_descriptor->virtual_functions, semantics),
        std::move(interfaces),
        file.type_descriptor->mangled_name};
  }

  std::vector<ImportedStaticFieldAbi> static_fields;
  static_fields.reserve(file.static_fields.size());
  for (const AbiStaticField& field : file.static_fields) {
    static_fields.push_back(
        ImportedStaticFieldAbi{member_identity(field.symbol, semantics),
                               canonical_type_identity(field.type, semantics),
                               field.linkage, field.mangled_name});
  }
  std::ranges::sort(static_fields, {},
                    &ImportedStaticFieldAbi::member_identity);

  std::vector<ImportedCallableAbi> callables;
  callables.reserve(file.functions.size() + file.constructors.size());
  for (const AbiCallable& callable : file.functions) {
    callables.push_back(import_callable(callable, semantics));
  }
  for (const AbiCallable& callable : file.constructors) {
    callables.push_back(import_callable(callable, semantics));
  }
  std::ranges::sort(callables, {}, &ImportedCallableAbi::member_identity);

  return ImportedClassAbi{file.layout.header_size, file.layout.size,
                          file.layout.alignment,   std::move(fields),
                          std::move(descriptor),   std::move(static_fields),
                          std::move(callables)};
}

std::optional<ImportedType> import_type(TypeId id,
                                        const SemanticModel& semantics,
                                        const AbiModule& abi) {
  const auto layout = std::ranges::find(abi.types, id, &AbiTypeLayout::type);
  if (layout == abi.types.end()) {
    return std::nullopt;
  }
  const SemanticType& type = semantics.type(id);
  std::optional<std::string> element;
  if (type.element_type) {
    element = canonical_type_identity(*type.element_type, semantics);
  }
  std::optional<NominalIdentity> nominal;
  if (type.file) {
    nominal = semantics.file(*type.file).identity;
  }
  return ImportedType{canonical_type_identity(id, semantics),
                      type.kind,
                      type.name,
                      std::move(element),
                      std::move(nominal),
                      layout->kind,
                      layout->bit_width,
                      layout->storage,
                      layout->reference_offsets};
}

std::optional<std::string> overload_type_identity(
    std::string_view identity,
    const std::map<std::string, const ImportedType*>& types,
    std::size_t depth = 0) {
  if (depth > types.size()) return std::nullopt;
  const auto type = types.find(std::string{identity});
  if (type == types.end()) return std::nullopt;
  if (type->second->kind == TypeKind::kNullable) {
    if (!type->second->element_identity) return std::nullopt;
    return overload_type_identity(*type->second->element_identity, types,
                                  depth + 1);
  }
  if (type->second->kind == TypeKind::kArray) {
    if (!type->second->element_identity) return std::nullopt;
    const auto element = overload_type_identity(*type->second->element_identity,
                                                types, depth + 1);
    if (!element) return std::nullopt;
    return canonical_array_identity(*element);
  }
  return std::string{identity};
}

std::optional<std::vector<std::string>> parameter_type_identities(
    const ImportedMember& member,
    const std::map<std::string, const ImportedType*>& types) {
  std::vector<std::string> parameters;
  parameters.reserve(member.parameters.size());
  for (const ImportedParameter& parameter : member.parameters) {
    const auto identity =
        overload_type_identity(parameter.type_identity, types);
    if (!identity) return std::nullopt;
    parameters.push_back(*identity);
  }
  return parameters;
}

CanonicalMemberKind canonical_member_kind(const ImportedMember& member) {
  switch (member.kind) {
    case ImportedMemberKind::kField:
      return member.is_static ? CanonicalMemberKind::kStaticField
                              : CanonicalMemberKind::kInstanceField;
    case ImportedMemberKind::kFunction:
      return CanonicalMemberKind::kFunction;
    case ImportedMemberKind::kConstructor:
      return CanonicalMemberKind::kConstructor;
  }
  return CanonicalMemberKind::kFunction;
}

bool sorted_unique_types(const std::vector<ImportedType>& values) {
  return std::ranges::is_sorted(values, {}, &ImportedType::identity) &&
         std::adjacent_find(
             values.begin(), values.end(),
             [](const ImportedType& left, const ImportedType& right) {
               return left.identity == right.identity;
             }) == values.end();
}

bool sorted_unique_files(const std::vector<ImportedFile>& values) {
  return std::ranges::is_sorted(values, {}, &ImportedFile::identity) &&
         std::adjacent_find(
             values.begin(), values.end(),
             [](const ImportedFile& left, const ImportedFile& right) {
               return left.identity == right.identity;
             }) == values.end();
}

template <typename Value, typename Projection>
bool sorted_unique(const std::vector<Value>& values, Projection projection) {
  return std::ranges::is_sorted(values, {}, projection) &&
         std::adjacent_find(values.begin(), values.end(),
                            [&](const Value& left, const Value& right) {
                              return std::invoke(projection, left) ==
                                     std::invoke(projection, right);
                            }) == values.end();
}

std::uint32_t pointer_bit_width(const TargetDataLayout& target) {
  if (target.pointer.size > std::numeric_limits<std::uint32_t>::max() / 8U) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(target.pointer.size * 8U);
}

AbiTypeLayout expected_type_layout(const ImportedType& type,
                                   const TargetDataLayout& target) {
  const auto make = [&](AbiTypeKind kind, std::uint32_t bit_width,
                        std::uint64_t size, std::uint64_t alignment) {
    return AbiTypeLayout{TypeId{0}, kind, bit_width,
                         SizeAlignment{size, alignment}};
  };
  switch (type.kind) {
    case TypeKind::kStruct:
      return make(AbiTypeKind::kAggregate, 0, type.storage.size,
                  type.storage.alignment);
    case TypeKind::kError:
      return make(AbiTypeKind::kInvalid, 0, 0, 1);
    case TypeKind::kVoid:
      return make(AbiTypeKind::kVoid, 0, 0, 1);
    case TypeKind::kNull:
    case TypeKind::kString:
    case TypeKind::kObject:
    case TypeKind::kFileClass:
    case TypeKind::kInterface:
    case TypeKind::kArray:
    case TypeKind::kNullable:
      return make(AbiTypeKind::kReference, pointer_bit_width(target),
                  target.pointer.size, target.pointer.alignment);
    case TypeKind::kBool:
      return make(AbiTypeKind::kInteger, 1, 1, 1);
    case TypeKind::kChar:
      return make(AbiTypeKind::kInteger, 32, 4, 4);
    case TypeKind::kByte:
    case TypeKind::kInt8:
    case TypeKind::kUint8:
      return make(AbiTypeKind::kInteger, 8, 1, 1);
    case TypeKind::kInt16:
    case TypeKind::kUint16:
      return make(AbiTypeKind::kInteger, 16, 2, 2);
    case TypeKind::kInt32:
    case TypeKind::kUint32:
    case TypeKind::kEnum:
      return make(AbiTypeKind::kInteger, 32, 4, 4);
    case TypeKind::kInt64:
    case TypeKind::kUint64:
      return make(AbiTypeKind::kInteger, 64, 8, target.int64_alignment);
    case TypeKind::kFloat32:
      return make(AbiTypeKind::kFloat, 32, 4, 4);
    case TypeKind::kFloat64:
      return make(AbiTypeKind::kFloat, 64, 8, target.float64_alignment);
  }
  return make(AbiTypeKind::kInvalid, 0, 0, 1);
}

void verify_type(const ImportedType& type, const TargetDataLayout& target,
                 const std::set<std::string>& type_identities,
                 IssueCollector& issues) {
  const std::string record = "type " + mangle_canonical_identity(type.identity);
  std::string expected_identity;
  if (is_nominal(type.kind)) {
    if (!type.nominal_identity || type.element_identity ||
        !valid_nominal_identity(*type.nominal_identity)) {
      issues.add(record, "nominal type record is malformed");
      return;
    }
    const NominalKind expected_kind =
        type.kind == TypeKind::kStruct      ? NominalKind::kStruct
        : type.kind == TypeKind::kEnum      ? NominalKind::kEnum
        : type.kind == TypeKind::kInterface ? NominalKind::kInterface
                                            : NominalKind::kClass;
    if (type.nominal_identity->kind != expected_kind) {
      issues.add(record, "semantic and nominal kinds disagree");
    }
    expected_identity = canonical_nominal_identity(*type.nominal_identity);
  } else if (is_structural(type.kind)) {
    if (!type.element_identity || type.nominal_identity ||
        !type_identities.contains(*type.element_identity)) {
      issues.add(record, "structural element type is missing or invalid");
      return;
    }
    expected_identity =
        type.kind == TypeKind::kArray
            ? canonical_array_identity(*type.element_identity)
            : canonical_nullable_identity(*type.element_identity);
  } else {
    if (type.element_identity || type.nominal_identity ||
        type.kind == TypeKind::kError) {
      issues.add(record, "primitive type record is malformed");
      return;
    }
    expected_identity = canonical_primitive_identity(type_kind_name(type.kind));
  }
  if (type.identity != expected_identity) {
    issues.add(record, "canonical type identity does not match its record");
  }
  if ((type.kind == TypeKind::kStruct && type.storage.size > kMaxStructSize) ||
      type.reference_offsets.size() > kMaxLayoutReferences) {
    issues.add(record,
               "aggregate value or reference map exceeds resource limits",
               ImportedPackageIssueCode::kLimitExceeded);
    return;
  }
  if (type.kind == TypeKind::kStruct &&
      (type.storage.size == 0 || !is_power_of_two(type.storage.alignment) ||
       type.storage.size % type.storage.alignment != 0)) {
    issues.add(record,
               "aggregate value size or alignment exceeds the contract");
  }
  const std::vector<std::uint64_t> scalar_references =
      type.abi_kind == AbiTypeKind::kReference ? std::vector<std::uint64_t>{0}
                                               : std::vector<std::uint64_t>{};
  if (type.kind != TypeKind::kStruct &&
      type.reference_offsets != scalar_references) {
    issues.add(record, "scalar value reference map is invalid");
  }
  if (!std::ranges::is_sorted(type.reference_offsets) ||
      std::adjacent_find(type.reference_offsets.begin(),
                         type.reference_offsets.end()) !=
          type.reference_offsets.end()) {
    issues.add(record, "value reference map is not sorted and unique");
  }
  for (const auto offset : type.reference_offsets) {
    if (target.pointer.alignment == 0 ||
        offset % target.pointer.alignment != 0 ||
        type.storage.alignment < target.pointer.alignment ||
        offset > type.storage.size ||
        target.pointer.size > type.storage.size - offset) {
      issues.add(record, "value reference slot lies outside aligned storage");
      break;
    }
  }
  const AbiTypeLayout expected = expected_type_layout(type, target);
  if (type.abi_kind != expected.kind || type.bit_width != expected.bit_width ||
      type.storage != expected.storage) {
    issues.add(record, "ABI type layout does not match the target contract");
  }
}

void verify_member(const ImportedFile& file, const ImportedMember& member,
                   const std::set<std::string>& type_identities,
                   const std::map<std::string, const ImportedType*>& types,
                   IssueCollector& issues) {
  const std::string record =
      "member " + mangle_canonical_identity(member.identity);
  if (member.owner_identity != file.identity ||
      !is_valid_identifier(member.name) ||
      member.visibility != infer_visibility(member.name) ||
      !type_identities.contains(member.type_identity) ||
      member.location.path != file.logical_path || member.location.line == 0 ||
      member.location.column == 0) {
    issues.add(record,
               "declaration ownership, spelling, type, or location is invalid");
  }
  for (const ImportedParameter& parameter : member.parameters) {
    if (!is_valid_identifier(parameter.name) ||
        !type_identities.contains(parameter.type_identity)) {
      issues.add(record, "parameter record is invalid");
    }
  }
  const auto parameters = parameter_type_identities(member, types);
  if (!parameters ||
      member.identity != canonical_member_identity(
                             file.nominal_identity,
                             canonical_member_kind(member), member.name,
                             parameters.value_or(std::vector<std::string>{}))) {
    issues.add(record, "canonical member identity does not match its record");
  }
  if (member.kind != ImportedMemberKind::kFunction &&
      (member.virtual_slot || member.overridden_identity ||
       member.is_override || member.is_abstract)) {
    issues.add(record, "non-function carries function dispatch metadata");
  }
  if (member.kind == ImportedMemberKind::kFunction) {
    if (member.is_override &&
        (file.kind != FileTypeKind::kClass || member.is_static ||
         member.visibility != Visibility::kPublic)) {
      issues.add(record, "override requires a public class instance function");
    }
    if (member.overridden_identity && !member.is_override) {
      issues.add(record, "replaced class function requires override");
    }
    if (member.is_final && (!member.is_override || member.is_abstract)) {
      issues.add(record, "final function requires a concrete override");
    }
  }
  if (member.kind == ImportedMemberKind::kConstructor && member.is_static) {
    issues.add(record, "constructor is marked static");
  }
  if (member.kind != ImportedMemberKind::kConstructor &&
      member.base_constructor_identity) {
    issues.add(record, "non-constructor selects a base constructor");
  }
  if (member.kind != ImportedMemberKind::kField && member.static_value) {
    issues.add(record, "non-field carries a static literal");
  }
  if (member.kind == ImportedMemberKind::kField && member.is_static &&
      (!member.is_final || !member.static_value)) {
    issues.add(record, "static field is not a final scalar constant");
  }
  if (member.static_value &&
      (member.static_value->kind == LiteralKind::kString ||
       member.static_value->kind == LiteralKind::kNull)) {
    issues.add(record, "static field carries a non-scalar literal");
  }
  if (member.kind == ImportedMemberKind::kField && !member.is_static &&
      member.static_value) {
    issues.add(record, "instance field carries a static literal");
  }
}

void verify_class_abi(
    const ImportedFile& file,
    const std::map<std::string, const ImportedMember*>& members,
    const std::map<std::string, const ImportedType*>& types,
    IssueCollector& issues) {
  const std::string record = "ABI " + file.logical_path;
  const ImportedClassAbi& abi = file.abi;
  if (abi.alignment == 0 || !is_power_of_two(abi.alignment) ||
      abi.header_size > abi.size || abi.size % abi.alignment != 0) {
    issues.add(record, "class size or alignment is invalid");
  }
  const bool aggregate = file.kind == FileTypeKind::kStruct;
  if ((aggregate && abi.fields.size() > kMaxStructFields) ||
      (abi.descriptor &&
       abi.descriptor->reference_offsets.size() > kMaxLayoutReferences)) {
    issues.add(record, "field or reference-map count exceeds layout limits",
               ImportedPackageIssueCode::kLimitExceeded);
    return;
  }
  if (aggregate ? abi.descriptor.has_value() : !abi.descriptor.has_value()) {
    issues.add(record, "heap descriptor presence disagrees with nominal kind");
    return;
  }
  if (abi.descriptor) {
    const ImportedTypeDescriptor& descriptor = *abi.descriptor;
    const std::string descriptor_identity = canonical_member_identity(
        file.nominal_identity, CanonicalMemberKind::kDescriptor, "");
    if (descriptor.kind != AbiHeapObjectKind::kFileClass ||
        descriptor.identity != descriptor_identity ||
        descriptor.display_name !=
            nominal_display_name(file.nominal_identity) ||
        descriptor.size != abi.size || descriptor.alignment != abi.alignment ||
        descriptor.parent_identity != file.base_identity) {
      issues.add(record, "descriptor does not match the owning file layout");
    }
    const std::string expected_descriptor_name =
        file.kind == FileTypeKind::kClass
            ? mangle_canonical_identity(descriptor_identity)
            : std::string{};
    if (descriptor.mangled_name != expected_descriptor_name) {
      issues.add(record, "descriptor linkage name is not canonical");
    }
  }
  std::set<std::uint64_t> offsets;
  for (const ImportedFieldLayout& field : abi.fields) {
    const auto type = types.find(field.type_identity);
    if (field.field_identity.empty() || type == types.end() ||
        field.offset < abi.header_size || field.offset >= abi.size ||
        type->second->storage.alignment == 0 ||
        field.offset % type->second->storage.alignment != 0 ||
        type->second->storage.size > abi.size - field.offset ||
        !offsets.insert(field.offset).second) {
      issues.add(record, "field layout entry is invalid");
    }
    const auto declaration = members.find(field.field_identity);
    if (declaration != members.end() &&
        (declaration->second->kind != ImportedMemberKind::kField ||
         declaration->second->is_static ||
         declaration->second->type_identity != field.type_identity)) {
      issues.add(record, "owned field layout disagrees with its declaration");
    }
  }
  std::vector<std::uint64_t> expected_references;
  for (const ImportedFieldLayout& field : abi.fields) {
    const auto type = types.find(field.type_identity);
    if (type != types.end()) {
      for (const auto offset : type->second->reference_offsets) {
        if (expected_references.size() == kMaxLayoutReferences) {
          issues.add(record, "flattened reference map exceeds layout limits",
                     ImportedPackageIssueCode::kLimitExceeded);
          return;
        }
        if (offset > abi.size || field.offset > abi.size - offset) {
          issues.add(record, "flattened reference slot exceeds layout bounds");
          return;
        }
        expected_references.push_back(field.offset + offset);
      }
    }
  }
  const auto own_type = types.find(file.identity);
  if ((abi.descriptor &&
       abi.descriptor->reference_offsets != expected_references) ||
      (aggregate &&
       (own_type == types.end() ||
        own_type->second->reference_offsets != expected_references ||
        own_type->second->storage != SizeAlignment{abi.size, abi.alignment}))) {
    issues.add(record, "managed-reference offsets do not match field layout");
  }

  if (!sorted_unique(abi.static_fields,
                     &ImportedStaticFieldAbi::member_identity) ||
      !sorted_unique(abi.callables, &ImportedCallableAbi::member_identity)) {
    issues.add(record, "ABI members are not in canonical unique order");
  }
  for (const ImportedStaticFieldAbi& field : abi.static_fields) {
    const auto member = members.find(field.member_identity);
    if (member == members.end() || !member->second->is_static ||
        member->second->kind != ImportedMemberKind::kField ||
        member->second->type_identity != field.type_identity ||
        field.linkage != (member->second->visibility == Visibility::kPublic
                              ? AbiLinkage::kExternal
                              : AbiLinkage::kInternal) ||
        field.mangled_name !=
            mangle_canonical_identity(field.member_identity)) {
      issues.add(record, "static-field ABI does not match its declaration");
    }
  }
  std::size_t declared_static_fields = 0;
  for (const ImportedMember& member : file.members) {
    if (member.kind == ImportedMemberKind::kField && member.is_static) {
      ++declared_static_fields;
    }
  }
  if (declared_static_fields != abi.static_fields.size()) {
    issues.add(record, "static-field ABI is incomplete");
  }
  for (const ImportedCallableAbi& callable : abi.callables) {
    const auto member = members.find(callable.member_identity);
    if (member == members.end()) {
      issues.add(record, "callable ABI has no owned declaration");
      continue;
    }
    const ImportedMember& declaration = *member->second;
    if (declaration.kind == ImportedMemberKind::kField) {
      issues.add(record, "callable ABI resolves to a field declaration");
      continue;
    }
    const bool constructor =
        declaration.kind == ImportedMemberKind::kConstructor;
    const AbiLinkage expected_linkage =
        declaration.visibility == Visibility::kPublic ? AbiLinkage::kExternal
                                                      : AbiLinkage::kInternal;
    if (callable.kind != (constructor ? AbiCallableKind::kConstructor
                                      : AbiCallableKind::kFunction) ||
        callable.linkage != expected_linkage ||
        callable.calling_convention != AbiCallingConvention::kC ||
        callable.mangled_name !=
            mangle_canonical_identity(callable.member_identity)) {
      issues.add(record, "callable ABI does not match its declaration");
    }
    if (constructor && !aggregate) {
      const auto parameters = parameter_type_identities(declaration, types);
      const std::string initializer = canonical_member_identity(
          file.nominal_identity, CanonicalMemberKind::kConstructorInitializer,
          declaration.name, parameters.value_or(std::vector<std::string>{}));
      if (!parameters || callable.initializer_identity != initializer ||
          callable.initializer_mangled_name !=
              mangle_canonical_identity(initializer) ||
          callable.initializer_linkage != expected_linkage) {
        issues.add(record,
                   "constructor initializer ABI is not canonically owned");
      }
      const std::string void_identity = canonical_primitive_identity("void");
      if (callable.initializer_return_type_identity != void_identity ||
          callable.initializer_parameters.size() !=
              declaration.parameters.size() + 1 ||
          !types.contains(void_identity) ||
          callable.initializer_return_mode != AbiReturnMode::kVoid ||
          callable.initializer_receiver_mode != AbiReceiverMode::kReference) {
        issues.add(record, "constructor initializer signature is incomplete");
      } else {
        const ImportedAbiParameter& receiver =
            callable.initializer_parameters.front();
        if (receiver.kind != AbiParameterKind::kReceiver ||
            receiver.type_identity != file.identity ||
            receiver.passing != AbiPassingMode::kDirect) {
          issues.add(record,
                     "constructor initializer receiver ABI is inconsistent");
        }
        for (std::size_t index = 0; index < declaration.parameters.size();
             ++index) {
          const ImportedAbiParameter& parameter =
              callable.initializer_parameters[index + 1];
          if (parameter.kind != AbiParameterKind::kExplicit ||
              parameter.type_identity !=
                  declaration.parameters[index].type_identity ||
              parameter.passing !=
                  (types.contains(parameter.type_identity) &&
                           types.at(parameter.type_identity)->kind ==
                               TypeKind::kStruct
                       ? AbiPassingMode::kValuePointer
                       : AbiPassingMode::kDirect)) {
            issues.add(record,
                       "constructor initializer parameter ABI is inconsistent");
          }
        }
      }
    } else if (callable.initializer_identity ||
               !callable.initializer_mangled_name.empty() ||
               callable.initializer_return_type_identity ||
               !callable.initializer_parameters.empty() ||
               callable.initializer_receiver_mode != AbiReceiverMode::kNone ||
               callable.initializer_return_mode != AbiReturnMode::kVoid) {
      issues.add(record, "function carries constructor initializer ABI");
    }
    const std::string& expected_return =
        constructor ? file.identity : declaration.type_identity;
    const auto returned = types.find(expected_return);
    if (returned == types.end()) {
      issues.add(record, "callable return type is absent");
      continue;
    }
    const AbiReturnMode return_mode =
        returned->second->kind == TypeKind::kVoid     ? AbiReturnMode::kVoid
        : returned->second->kind == TypeKind::kStruct ? AbiReturnMode::kIndirect
                                                      : AbiReturnMode::kDirect;
    const bool has_receiver = !constructor && !declaration.is_static;
    const AbiReceiverMode receiver_mode =
        constructor && aggregate ? AbiReceiverMode::kConstruction
        : !has_receiver          ? AbiReceiverMode::kNone
        : aggregate              ? AbiReceiverMode::kReadOnlyValue
                                 : AbiReceiverMode::kReference;
    std::vector<ImportedAbiParameter> expected_parameters;
    if (return_mode == AbiReturnMode::kIndirect) {
      expected_parameters.push_back({AbiParameterKind::kResult, expected_return,
                                     AbiPassingMode::kResultPointer});
    }
    if (has_receiver) {
      expected_parameters.push_back({AbiParameterKind::kReceiver, file.identity,
                                     aggregate ? AbiPassingMode::kValuePointer
                                               : AbiPassingMode::kDirect});
    }
    for (const auto& parameter : declaration.parameters) {
      const auto type = types.find(parameter.type_identity);
      expected_parameters.push_back(
          {AbiParameterKind::kExplicit, parameter.type_identity,
           type != types.end() && type->second->kind == TypeKind::kStruct
               ? AbiPassingMode::kValuePointer
               : AbiPassingMode::kDirect});
    }
    if (callable.parameters != expected_parameters ||
        callable.return_type_identity != expected_return ||
        callable.return_mode != return_mode ||
        callable.receiver_mode != receiver_mode) {
      issues.add(record,
                 "callable physical signature disagrees with declarations");
    }
  }

  std::size_t declared_callables = 0;
  for (const ImportedMember& member : file.members) {
    if (member.kind == ImportedMemberKind::kFunction ||
        member.kind == ImportedMemberKind::kConstructor) {
      ++declared_callables;
    }
  }
  if (declared_callables != abi.callables.size()) {
    issues.add(record, "callable ABI is incomplete");
  }
  if (file.kind == FileTypeKind::kInterface &&
      (!abi.fields.empty() || !abi.static_fields.empty())) {
    issues.add(record, "interface contains storage ABI");
  }

  if (!abi.descriptor) return;
  const ImportedTypeDescriptor& descriptor = *abi.descriptor;
  if (descriptor.virtual_function_identities !=
      file.virtual_function_identities) {
    issues.add(record, "descriptor virtual table disagrees with semantics");
  }

  std::uint64_t previous_interface = 0;
  bool first_interface = true;
  for (const ImportedInterfaceDispatch& interface_dispatch :
       descriptor.interfaces) {
    if ((!first_interface &&
         interface_dispatch.interface_id <= previous_interface) ||
        interface_dispatch.interface_identity.empty()) {
      issues.add(record, "interface dispatch entries are not canonical");
    }
    const auto interface_type =
        types.find(interface_dispatch.interface_identity);
    if (interface_type == types.end() ||
        interface_type->second->kind != TypeKind::kInterface ||
        !interface_type->second->nominal_identity ||
        interface_dispatch.interface_id !=
            canonical_interface_id(*interface_type->second->nominal_identity)) {
      issues.add(record, "interface dispatch identity or ID is invalid");
    }
    const auto implementation = std::ranges::find(
        file.interface_implementations, interface_dispatch.interface_identity,
        &ImportedInterfaceImplementation::interface_identity);
    if (implementation == file.interface_implementations.end() ||
        implementation->function_identities !=
            interface_dispatch.function_identities) {
      issues.add(record,
                 "descriptor dispatch disagrees with semantic conformance");
    }
    first_interface = false;
    previous_interface = interface_dispatch.interface_id;
  }
  if (descriptor.interfaces.size() != file.interface_implementations.size()) {
    issues.add(record, "descriptor interface dispatch set is incomplete");
  }
}

// Use declarations, not the supplied dispatch tables, to establish override
// intent. A standalone artifact may refer to foreign declarations; those
// obligations are checked once the dependency closure is available.
void verify_override_contracts(
    std::span<const ImportedPackageView* const> packages, bool require_owners,
    IssueCollector& issues) {
  std::map<std::string, const ImportedFile*> files;
  for (const auto* package : packages) {
    for (const auto& file : package->files) files.emplace(file.identity, &file);
  }
  const auto is_instance_function = [](const ImportedMember& member) {
    return member.kind == ImportedMemberKind::kFunction && !member.is_static &&
           member.visibility == Visibility::kPublic;
  };
  const auto matches = [&](const ImportedMember& candidate,
                           const ImportedMember& member) {
    return is_instance_function(candidate) && candidate.name == member.name &&
           std::ranges::equal(candidate.parameters, member.parameters, {},
                              &ImportedParameter::type_identity,
                              &ImportedParameter::type_identity);
  };
  for (const auto& [identity, file] : files) {
    if (file->kind != FileTypeKind::kClass) continue;
    const std::string record = "overrides " + file->logical_path;
    std::vector<const ImportedFile*> bases;
    std::vector<std::string> pending_interfaces =
        file->direct_interface_identities;
    bool bases_complete = true;
    std::set<std::string> visited{identity};
    auto base_identity = file->base_identity;
    while (base_identity) {
      if (!visited.insert(*base_identity).second) {
        issues.add(record, "class override hierarchy contains a cycle");
        bases_complete = false;
        break;
      }
      const auto base = files.find(*base_identity);
      if (base == files.end()) {
        if (require_owners) issues.add(record, "base declaration is missing");
        bases_complete = false;
        break;
      }
      bases.push_back(base->second);
      const auto& interfaces = base->second->direct_interface_identities;
      pending_interfaces.insert(pending_interfaces.end(), interfaces.begin(),
                                interfaces.end());
      base_identity = base->second->base_identity;
    }
    std::vector<const ImportedFile*> interfaces;
    bool interfaces_complete = bases_complete;
    visited.clear();
    while (!pending_interfaces.empty()) {
      std::string next = std::move(pending_interfaces.back());
      pending_interfaces.pop_back();
      if (!visited.insert(next).second) continue;
      const auto parent = files.find(next);
      if (parent == files.end()) {
        if (require_owners) {
          issues.add(record, "interface declaration is missing");
        }
        interfaces_complete = false;
        continue;
      }
      interfaces.push_back(parent->second);
      const auto& parents = parent->second->direct_interface_identities;
      pending_interfaces.insert(pending_interfaces.end(), parents.begin(),
                                parents.end());
    }
    for (const auto& member : file->members) {
      if (!is_instance_function(member)) continue;
      const ImportedMember* replaced = nullptr;
      for (const auto* base : bases) {
        const auto found = std::ranges::find_if(
            base->members,
            [&](const auto& candidate) { return matches(candidate, member); });
        if (found != base->members.end()) {
          replaced = &*found;
          break;
        }
      }
      const bool implements =
          std::ranges::any_of(interfaces, [&](const auto* parent) {
            return std::ranges::any_of(parent->members,
                                       [&](const auto& candidate) {
                                         return matches(candidate, member);
                                       });
          });
      const std::string member_record = record + ": " + member.name;
      if ((replaced || implements) && !member.is_override) {
        issues.add(member_record,
                   "class or interface implementation requires override");
      }
      if (!replaced && !implements && interfaces_complete &&
          member.is_override) {
        issues.add(member_record,
                   "override has no class or interface contract");
      }
      if (replaced && replaced->is_final) {
        issues.add(member_record, "override replaces a final function");
      }
      if ((replaced || bases_complete) &&
          member.overridden_identity !=
              (replaced ? std::optional{replaced->identity} : std::nullopt)) {
        issues.add(member_record,
                   "replaced class function does not match override target");
      }
    }
  }
}

// Local artifacts may claim dependency-owned value layouts. Those claims are
// shape-checked locally, then matched against their owners in the full closure.
// The graph contains only inline fields and class bases, never managed edges.
void verify_layout_graph(std::span<const ImportedPackageView* const> packages,
                         bool require_owners, IssueCollector& issues) {
  if (packages.empty()) return;
  const auto& target = packages.front()->target;
  if (!is_valid_data_layout(target)) return;
  std::map<std::string, const ImportedType*> types;
  std::map<std::string, const ImportedFile*> files;
  for (const auto* package : packages) {
    if (package->target != target) {
      issues.add("closure", "package target layouts disagree");
      return;
    }
    for (const auto& type : package->types) {
      const auto [entry, inserted] = types.emplace(type.identity, &type);
      if (!inserted && *entry->second != type) {
        issues.add("closure", "dependency-owned type claims disagree");
        return;
      }
    }
    for (const auto& file : package->files) {
      if (!files.emplace(file.identity, &file).second) {
        issues.add("closure", "nominal declaration has multiple owners");
        return;
      }
    }
  }
  for (const auto& [id, type] : types) {
    if (require_owners && is_nominal(type->kind) && !files.contains(id)) {
      issues.add("closure",
                 "nominal type has no declaration in dependency closure");
      return;
    }
    if (type->kind == TypeKind::kStruct &&
        (type->storage.size > kMaxStructSize ||
         type->reference_offsets.size() > kMaxLayoutReferences)) {
      issues.add("layout", "aggregate type exceeds resource limits",
                 ImportedPackageIssueCode::kLimitExceeded);
      return;
    }
  }
  std::map<std::string, std::size_t> indegrees;
  std::map<std::string, std::vector<std::string>> dependents;
  for (const auto& [id, file] : files) {
    std::set<std::string> dependencies;
    if (file->base_identity && files.contains(*file->base_identity)) {
      dependencies.insert(*file->base_identity);
    }
    for (const auto& member : file->members) {
      if (member.kind != ImportedMemberKind::kField || member.is_static)
        continue;
      const auto type = types.find(member.type_identity);
      if (type != types.end() && type->second->kind == TypeKind::kStruct &&
          files.contains(member.type_identity)) {
        dependencies.insert(member.type_identity);
      }
    }
    indegrees.emplace(id, dependencies.size());
    for (const auto& dependency : dependencies)
      dependents[dependency].push_back(id);
  }
  std::set<std::string> ready;
  for (const auto& [id, degree] : indegrees) {
    if (degree == 0) ready.insert(id);
  }
  std::map<std::string, std::size_t> depths;
  std::size_t visited = 0;
  std::size_t map_entries = 0;
  while (!ready.empty()) {
    const auto id = *ready.begin();
    ready.erase(ready.begin());
    const auto& file = *files.at(id);
    ++visited;
    const bool aggregate = file.kind == FileTypeKind::kStruct;
    const auto record = "layout " + file.logical_path;
    const auto fail = [&](std::string message,
                          ImportedPackageIssueCode code =
                              ImportedPackageIssueCode::kInvalidModel) {
      issues.add(record, std::move(message), code);
    };
    const auto& abi = file.abi;
    if (abi.fields.size() > kMaxStructFields && aggregate) {
      fail("struct exceeds the field-count limit",
           ImportedPackageIssueCode::kLimitExceeded);
      return;
    }
    std::size_t depth = aggregate ? 1 : 0;
    if (file.kind != FileTypeKind::kEnum) {
      const std::uint64_t header =
          aggregate ? 0 : target.pointer.size * target.object_header_words;
      std::uint64_t offset = header;
      std::uint64_t alignment = aggregate ? 1 : target.pointer.alignment;
      std::vector<ImportedFieldLayout> fields;
      bool complete_base = true;
      if (file.base_identity) {
        const auto base = files.find(*file.base_identity);
        if (base == files.end()) {
          complete_base = false;
        } else {
          offset = base->second->abi.size;
          alignment = std::max(alignment, base->second->abi.alignment);
          fields = base->second->abi.fields;
        }
      }
      std::map<std::string, const ImportedMember*> members;
      for (const auto& member : file.members)
        members.emplace(member.identity, &member);
      for (const auto& member_id : file.member_order) {
        const auto member = members.find(member_id);
        if (member == members.end()) {
          fail("field declaration order references an unknown member");
          return;
        }
        const auto& declaration = *member->second;
        if (declaration.kind != ImportedMemberKind::kField ||
            declaration.is_static)
          continue;
        const auto found = types.find(declaration.type_identity);
        if (found == types.end()) {
          fail("field type is absent");
          return;
        }
        const auto& type = *found->second;
        if (type.storage.size == 0 ||
            !is_power_of_two(type.storage.alignment) ||
            offset > std::numeric_limits<std::uint64_t>::max() -
                         (type.storage.alignment - 1)) {
          fail("field storage is invalid or alignment overflows");
          return;
        }
        offset = (offset + type.storage.alignment - 1) &
                 ~(type.storage.alignment - 1);
        if (type.storage.size >
            std::numeric_limits<std::uint64_t>::max() - offset) {
          fail("field size overflows layout");
          return;
        }
        fields.push_back(
            {declaration.identity, declaration.type_identity, offset});
        offset += type.storage.size;
        alignment = std::max(alignment, type.storage.alignment);
        if (aggregate && type.kind == TypeKind::kStruct) {
          depth = std::max(depth, depths[declaration.type_identity] + 1);
        }
      }
      if (aggregate) offset = std::max(offset, std::uint64_t{1});
      if (offset >
          std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        fail("tail padding overflows layout");
        return;
      }
      const auto size = (offset + alignment - 1) & ~(alignment - 1);
      if (aggregate && (size > kMaxStructSize || depth > kMaxStructDepth)) {
        fail("aggregate layout exceeds size or nesting limits",
             ImportedPackageIssueCode::kLimitExceeded);
        return;
      }
      if (complete_base &&
          (abi.header_size != header || abi.size != size ||
           abi.alignment != alignment || abi.fields != fields)) {
        fail("layout does not match declaration-order reconstruction");
        return;
      }
    }
    depths.emplace(id, depth);
    const auto own_type = types.find(id);
    if (own_type == types.end()) {
      fail("nominal type is absent");
      return;
    }
    const std::size_t reference_count =
        aggregate        ? own_type->second->reference_offsets.size()
        : abi.descriptor ? abi.descriptor->reference_offsets.size()
                         : 0;
    if (reference_count > kMaxLayoutReferences ||
        reference_count > kMaxAggregateMapEntries - map_entries) {
      fail("flattened reference maps exceed closure resource limits",
           ImportedPackageIssueCode::kLimitExceeded);
      return;
    }
    map_entries += reference_count;
    for (const auto& dependent : dependents[id]) {
      if (--indegrees[dependent] == 0) ready.insert(dependent);
    }
  }
  if (visited != files.size()) {
    issues.add("layout", "inline storage dependency graph contains a cycle");
  }
}

}  // namespace

ImportedPackageResult build_imported_package_view(
    const PackageIdentity& package, const SemanticModel& semantics,
    const MirModule& mir, const AbiModule& abi) {
  ImportedPackageView view{package, abi.target, {}, {}};
  IssueCollector issues;
  if (!is_valid_package_name(package.name) ||
      !is_valid_package_version(package.version)) {
    issues.add("package", "package identity is invalid");
    return ImportedPackageResult{std::nullopt, issues.take()};
  }

  std::vector<FileId> owned_files;
  for (std::size_t index = 0; index < semantics.files().size(); ++index) {
    const FileId file_id{index};
    const FileSemantics& semantic_file = semantics.file(file_id);
    if (semantic_file.identity.package == package)
      owned_files.push_back(file_id);
  }
  if (owned_files.empty()) {
    issues.add("package", "package has no owned file declarations");
    return ImportedPackageResult{std::nullopt, issues.take()};
  }

  std::set<std::size_t> referenced_type_ids;
  const auto add_type = [&](this const auto& self, TypeId type) -> void {
    if (type.value >= semantics.types().size() ||
        !referenced_type_ids.insert(type.value).second) {
      return;
    }
    const SemanticType& semantic_type = semantics.type(type);
    if (semantic_type.element_type) self(*semantic_type.element_type);
  };
  const auto add_symbol_types = [&](SymbolId symbol_id) {
    const SemanticSymbol& symbol = semantics.symbol(symbol_id);
    add_type(symbol.type);
    for (const TypeId parameter : symbol.parameter_types) add_type(parameter);
  };
  for (const FileId file_id : owned_files) {
    const FileSemantics& file = semantics.file(file_id);
    add_type(file.type);
    if (file.base_file) add_type(semantics.file(*file.base_file).type);
    for (const FileId interface_file : file.interfaces) {
      add_type(semantics.file(interface_file).type);
    }
    for (const SymbolId member : file.fields) add_symbol_types(member);
    for (const SymbolId member : file.functions) add_symbol_types(member);
    for (const SymbolId member : file.constructors) add_symbol_types(member);
    if (!file.constructors.empty()) add_type(semantics.void_type());
    const AbiFileClass* abi_file = find_abi_file(abi, file_id);
    if (abi_file == nullptr) continue;
    for (const AbiFieldLayout& field : abi_file->layout.fields) {
      add_type(field.type);
    }
    for (const AbiStaticField& field : abi_file->static_fields) {
      add_type(field.type);
    }
    const auto add_callable_types = [&](const AbiCallable& callable) {
      add_type(callable.return_type);
      for (const AbiParameter& parameter : callable.parameters) {
        add_type(parameter.type);
      }
    };
    for (const AbiCallable& callable : abi_file->functions) {
      add_callable_types(callable);
    }
    for (const AbiCallable& callable : abi_file->constructors) {
      add_callable_types(callable);
    }
  }

  view.types.reserve(referenced_type_ids.size());
  for (const std::size_t index : referenced_type_ids) {
    const TypeId type{index};
    if (semantics.type(type).kind == TypeKind::kError) {
      issues.add("type", "owned package references the semantic error type");
      continue;
    }
    std::optional<ImportedType> imported = import_type(type, semantics, abi);
    if (!imported) {
      issues.add("type", "verified ABI is missing a semantic type layout");
      continue;
    }
    view.types.push_back(std::move(*imported));
  }
  std::ranges::sort(view.types, {}, &ImportedType::identity);

  for (const FileId file_id : owned_files) {
    const FileSemantics& semantic_file = semantics.file(file_id);
    const AbiFileClass* abi_file = find_abi_file(abi, file_id);
    const MirFileClass* mir_file = find_mir_file(mir, file_id);
    if (abi_file == nullptr || mir_file == nullptr) {
      issues.add("file " + semantic_file.identity.name,
                 "verified MIR or ABI file is missing");
      continue;
    }
    const std::string identity =
        canonical_nominal_identity(semantic_file.identity);
    const std::string path = logical_path(semantic_file.identity);
    std::vector<ImportedMember> members;
    members.reserve(semantic_file.fields.size() +
                    semantic_file.functions.size() +
                    semantic_file.constructors.size());
    for (const SymbolId member : semantic_file.fields) {
      members.push_back(
          import_member(member, identity, path, semantics, *mir_file));
    }
    for (const SymbolId member : semantic_file.functions) {
      members.push_back(
          import_member(member, identity, path, semantics, *mir_file));
    }
    for (const SymbolId member : semantic_file.constructors) {
      members.push_back(
          import_member(member, identity, path, semantics, *mir_file));
    }
    std::ranges::sort(members, {}, &ImportedMember::identity);

    std::vector<ImportedInterfaceImplementation> implementations;
    implementations.reserve(semantic_file.interface_implementations.size());
    for (const InterfaceImplementation& implementation :
         semantic_file.interface_implementations) {
      implementations.push_back(ImportedInterfaceImplementation{
          file_identity(implementation.interface_file, semantics),
          symbol_identities(implementation.functions, semantics)});
    }
    std::ranges::sort(implementations, {},
                      &ImportedInterfaceImplementation::interface_identity);

    view.files.push_back(ImportedFile{
        semantic_file.identity, identity, path,
        import_location(semantics.symbol(semantic_file.symbol), path),
        semantics.symbol(semantic_file.symbol).visibility, semantic_file.kind,
        semantic_file.is_abstract, semantic_file.is_sealed,
        semantic_file.base_file ? std::optional<std::string>{file_identity(
                                      *semantic_file.base_file, semantics)}
                                : std::nullopt,
        file_identities(semantic_file.direct_interfaces, semantics),
        file_identities(semantic_file.interfaces, semantics),
        semantic_file.interface_id, std::move(members),
        declaration_order(semantic_file, *abi_file, semantics),
        symbol_identities(semantic_file.virtual_functions, semantics),
        symbol_identities(semantic_file.abstract_functions, semantics),
        symbol_identities(semantic_file.interface_functions, semantics),
        std::move(implementations), import_class_abi(*abi_file, semantics)});
    for (const SymbolId case_id : semantic_file.enum_cases) {
      const SemanticSymbol& item = semantics.symbol(case_id);
      view.files.back().enum_cases.push_back(ImportedEnumCase{
          canonical_member_identity(semantic_file.identity,
                                    CanonicalMemberKind::kEnumCase, item.name),
          item.name, *item.enum_tag, import_location(item, path)});
    }
  }
  std::ranges::sort(view.files, {}, &ImportedFile::identity);

  std::vector<ImportedPackageIssue> validation =
      verify_imported_package_view(view);
  std::vector<ImportedPackageIssue> build_issues = issues.take();
  build_issues.insert(build_issues.end(),
                      std::make_move_iterator(validation.begin()),
                      std::make_move_iterator(validation.end()));
  if (!build_issues.empty()) {
    return ImportedPackageResult{std::nullopt, std::move(build_issues)};
  }
  return ImportedPackageResult{std::move(view), {}};
}

std::vector<ImportedPackageIssue> verify_imported_package_view(
    const ImportedPackageView& view) {
  IssueCollector issues;
  if (!is_valid_package_name(view.package.name) ||
      !is_valid_package_version(view.package.version)) {
    issues.add("package", "package identity is invalid");
  }
  if (!is_valid_data_layout(view.target)) {
    issues.add("target", "target data layout is invalid");
  }
  if (!sorted_unique_types(view.types)) {
    issues.add("types", "types are not in canonical unique order");
  }
  if (!sorted_unique_files(view.files)) {
    issues.add("files", "files are not in canonical unique order");
  }
  if (view.files.empty()) {
    issues.add("files", "package contains no owned file declarations");
  }

  std::set<std::string> type_identities;
  std::map<std::string, const ImportedType*> types;
  for (const ImportedType& type : view.types) {
    type_identities.insert(type.identity);
    types.emplace(type.identity, &type);
  }
  for (const ImportedType& type : view.types) {
    verify_type(type, view.target, type_identities, issues);
    if (type.kind == TypeKind::kNullable && type.element_identity) {
      const auto element = types.find(*type.element_identity);
      if (element != types.end() &&
          (element->second->kind == TypeKind::kEnum ||
           element->second->kind == TypeKind::kStruct)) {
        issues.add("type",
                   "nullable enum/struct value types are not supported");
      }
    }
  }

  for (const ImportedFile& file : view.files) {
    const std::string record = "file " + file.logical_path;
    const NominalKind expected_nominal =
        file.kind == FileTypeKind::kStruct      ? NominalKind::kStruct
        : file.kind == FileTypeKind::kEnum      ? NominalKind::kEnum
        : file.kind == FileTypeKind::kInterface ? NominalKind::kInterface
                                                : NominalKind::kClass;
    if (!valid_nominal_identity(file.nominal_identity) ||
        file.nominal_identity.package != view.package ||
        file.nominal_identity.kind != expected_nominal ||
        file.identity != canonical_nominal_identity(file.nominal_identity) ||
        file.logical_path != logical_path(file.nominal_identity) ||
        file.location.path != file.logical_path || file.location.line == 0 ||
        file.location.column == 0 ||
        file.visibility != infer_visibility(file.nominal_identity.name)) {
      issues.add(
          record,
          "file identity, ownership, path, kind, or visibility is invalid");
    }
    const auto own_type = types.find(file.identity);
    if (own_type == types.end() ||
        own_type->second->kind !=
            (file.kind == FileTypeKind::kStruct      ? TypeKind::kStruct
             : file.kind == FileTypeKind::kEnum      ? TypeKind::kEnum
             : file.kind == FileTypeKind::kInterface ? TypeKind::kInterface
                                                     : TypeKind::kFileClass)) {
      issues.add(record, "owning nominal type is absent or has the wrong kind");
    }
    if (file.kind == FileTypeKind::kInterface) {
      if (!file.interface_id ||
          *file.interface_id != canonical_interface_id(file.nominal_identity) ||
          file.base_identity || file.is_sealed) {
        issues.add(record, "interface declaration metadata is invalid");
      }
    } else if (file.interface_id) {
      issues.add(record, "class carries an interface ID");
    }

    if (!sorted_unique(file.members, &ImportedMember::identity) ||
        !sorted_unique(file.interface_implementations,
                       &ImportedInterfaceImplementation::interface_identity)) {
      issues.add(record, "declarations or conformance maps are not canonical");
    }
    std::map<std::string, const ImportedMember*> members;
    for (const ImportedMember& member : file.members) {
      members.emplace(member.identity, &member);
      verify_member(file, member, type_identities, types, issues);
      if (file.kind == FileTypeKind::kStruct &&
          (member.is_abstract || member.is_override || member.virtual_slot ||
           member.overridden_identity || member.base_constructor_identity)) {
        issues.add(record,
                   "struct member carries inheritance or dispatch metadata");
      }
      if (member.is_static && member.kind == ImportedMemberKind::kField &&
          types.contains(member.type_identity) &&
          types.at(member.type_identity)->kind == TypeKind::kStruct) {
        issues.add(record, "aggregate static constants are not supported");
      }
      const auto type = types.find(member.type_identity);
      if (member.static_value && type != types.end() &&
          (member.static_value->kind == LiteralKind::kEnum ||
           type->second->kind == TypeKind::kEnum)) {
        const std::string& text = member.static_value->lexeme;
        std::uint32_t tag = 0;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), tag);
        const auto owner = std::ranges::find(view.files, member.type_identity,
                                             &ImportedFile::identity);
        if (member.static_value->kind != LiteralKind::kEnum ||
            type->second->kind != TypeKind::kEnum || text.empty() ||
            (text.size() > 1 && text.front() == '0') || error != std::errc{} ||
            end != text.data() + text.size() || tag >= kMaxEnumCases ||
            (owner != view.files.end() && tag >= owner->enum_cases.size())) {
          issues.add(record, "static enum constant has an invalid type or tag");
        }
      }
    }
    std::set<std::string> ordered;
    for (const std::string& member : file.member_order) {
      if (!members.contains(member) || !ordered.insert(member).second) {
        issues.add(record,
                   "member-order list has an unknown or duplicate entry");
      }
    }
    if (ordered.size() != members.size()) {
      issues.add(record, "member-order list does not cover every declaration");
    }
    if (file.base_identity) {
      const auto base = types.find(*file.base_identity);
      if (base == types.end() || base->second->kind != TypeKind::kFileClass) {
        issues.add(record, "base type is absent or is not a class");
      }
    }
    for (const std::string& interface_identity :
         file.direct_interface_identities) {
      const auto interface_type = types.find(interface_identity);
      if (interface_type == types.end() ||
          interface_type->second->kind != TypeKind::kInterface) {
        issues.add(record, "direct interface is absent or has the wrong kind");
      }
    }
    for (const std::string& interface_identity : file.interface_identities) {
      const auto interface_type = types.find(interface_identity);
      if (interface_type == types.end() ||
          interface_type->second->kind != TypeKind::kInterface) {
        issues.add(record, "interface closure is absent or has the wrong kind");
      }
    }
    if (file.kind == FileTypeKind::kStruct &&
        (file.is_abstract || file.is_sealed || file.base_identity ||
         !file.direct_interface_identities.empty() ||
         !file.interface_identities.empty() ||
         !file.virtual_function_identities.empty() ||
         !file.abstract_function_identities.empty() ||
         !file.interface_function_identities.empty() ||
         !file.interface_implementations.empty() ||
         file.abi.header_size != 0)) {
      issues.add(
          record,
          "struct declaration carries invalid storage or inheritance metadata");
    }
    if (file.kind == FileTypeKind::kEnum) {
      const auto& descriptor = file.abi.descriptor;
      if (!descriptor) {
        issues.add(record, "enum ABI placeholder descriptor is missing");
        continue;
      }
      if (file.enum_cases.empty() || file.enum_cases.size() > kMaxEnumCases ||
          !file.members.empty() || file.is_abstract || file.is_sealed ||
          file.base_identity || !file.direct_interface_identities.empty() ||
          !file.interface_identities.empty() ||
          !file.virtual_function_identities.empty() ||
          !file.abstract_function_identities.empty() ||
          !file.interface_function_identities.empty() ||
          !file.interface_implementations.empty() ||
          file.abi.header_size != 0 || file.abi.size != 0 ||
          file.abi.alignment != 1 || !file.abi.fields.empty() ||
          !file.abi.static_fields.empty() || !file.abi.callables.empty() ||
          descriptor->kind != AbiHeapObjectKind::kFileClass ||
          !descriptor->mangled_name.empty() || descriptor->parent_identity ||
          descriptor->size != 0 || descriptor->alignment != 1 ||
          !descriptor->reference_offsets.empty() ||
          !descriptor->virtual_function_identities.empty() ||
          !descriptor->interfaces.empty() ||
          descriptor->identity !=
              canonical_member_identity(file.nominal_identity,
                                        CanonicalMemberKind::kDescriptor, "") ||
          descriptor->display_name !=
              nominal_display_name(file.nominal_identity)) {
        issues.add(record,
                   "enum declaration or scalar ABI metadata is invalid");
      }
      std::set<std::string> names;
      for (std::size_t index = 0; index < file.enum_cases.size(); ++index) {
        const ImportedEnumCase& item = file.enum_cases[index];
        if (!is_valid_identifier(item.name) ||
            identifier_token_kind(item.name) != TokenKind::kIdentifier ||
            !names.insert(item.name).second || item.tag != index ||
            item.identity != canonical_member_identity(
                                 file.nominal_identity,
                                 CanonicalMemberKind::kEnumCase, item.name) ||
            item.location.path != file.logical_path ||
            item.location.line == 0 || item.location.column == 0) {
          issues.add(
              record,
              "enum case identity, name, tag, order, or location is invalid");
        }
      }
    } else {
      if (!file.enum_cases.empty())
        issues.add(record, "non-enum file contains enum cases");
      verify_class_abi(file, members, types, issues);
    }
  }
  const ImportedPackageView* single = &view;
  verify_layout_graph({&single, 1}, false, issues);
  verify_override_contracts({&single, 1}, false, issues);
  return issues.take();
}

std::vector<ImportedPackageIssue> verify_imported_package_closure(
    std::span<const ImportedPackageView* const> packages) {
  for (const auto* package : packages) {
    auto issues = verify_imported_package_view(*package);
    if (!issues.empty()) return issues;
  }
  IssueCollector issues;
  verify_layout_graph(packages, true, issues);
  verify_override_contracts(packages, true, issues);
  return issues.take();
}

std::string imported_callable_signature(
    AbiReturnMode return_mode, AbiReceiverMode receiver_mode,
    std::string_view return_type,
    std::span<const ImportedAbiParameter> parameters) {
  std::string result = "c:";
  result += abi_return_mode_name(return_mode);
  result += ':' + mangle_canonical_identity(return_type) + '(';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) result += ',';
    const auto& parameter = parameters[index];
    result += parameter.kind == AbiParameterKind::kResult     ? "result:"
              : parameter.kind == AbiParameterKind::kReceiver ? "receiver:"
                                                              : "explicit:";
    result += abi_passing_mode_name(parameter.passing);
    result += ':' + mangle_canonical_identity(parameter.type_identity);
  }
  result += ");receiver:";
  result += abi_receiver_mode_name(receiver_mode);
  return result;
}

}  // namespace cloth
