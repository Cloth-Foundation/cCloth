// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"

#include "cloth/identity/package_identity.h"
#include "cloth/sema/canonical_identity.h"

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
  void add(std::string record, std::string message) {
    issues_.push_back(
        ImportedPackageIssue{std::move(record), std::move(message)});
  }

  [[nodiscard]] std::vector<ImportedPackageIssue> take() {
    return std::move(issues_);
  }

 private:
  std::vector<ImportedPackageIssue> issues_;
};

bool is_nominal(TypeKind kind) {
  return kind == TypeKind::kFileClass || kind == TypeKind::kInterface ||
         kind == TypeKind::kEnum;
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
        parameter.kind, canonical_type_identity(parameter.type, semantics)});
  }
  std::optional<std::string> initializer_identity;
  std::optional<std::string> initializer_return_type;
  std::vector<ImportedAbiParameter> initializer_parameters;
  if (callable.kind == AbiCallableKind::kConstructor) {
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
      std::move(parameters)};
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

  std::vector<ImportedInterfaceDispatch> interfaces;
  interfaces.reserve(file.type_descriptor.interfaces.size());
  for (const AbiTypeDescriptor::InterfaceDispatch& interface_dispatch :
       file.type_descriptor.interfaces) {
    interfaces.push_back(ImportedInterfaceDispatch{
        file_identity(interface_dispatch.interface_file, semantics),
        interface_dispatch.interface_id,
        symbol_identities(interface_dispatch.functions, semantics)});
  }

  ImportedTypeDescriptor descriptor{
      file.type_descriptor.kind,
      canonical_member_identity(semantic_file.identity,
                                CanonicalMemberKind::kDescriptor, ""),
      file.type_descriptor.parent_file
          ? std::optional<std::string>{file_identity(
                *file.type_descriptor.parent_file, semantics)}
          : std::nullopt,
      file.type_descriptor.name,
      file.type_descriptor.size,
      file.type_descriptor.alignment,
      file.type_descriptor.reference_offsets,
      symbol_identities(file.type_descriptor.virtual_functions, semantics),
      std::move(interfaces),
      file.type_descriptor.mangled_name};

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
                      layout->storage};
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
        type.kind == TypeKind::kEnum        ? NominalKind::kEnum
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
  const ImportedTypeDescriptor& descriptor = abi.descriptor;
  const std::string descriptor_identity = canonical_member_identity(
      file.nominal_identity, CanonicalMemberKind::kDescriptor, "");
  if (descriptor.kind != AbiHeapObjectKind::kFileClass ||
      descriptor.identity != descriptor_identity ||
      descriptor.display_name != nominal_display_name(file.nominal_identity) ||
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
    if (type != types.end() &&
        type->second->abi_kind == AbiTypeKind::kReference) {
      expected_references.push_back(field.offset);
    }
  }
  if (descriptor.reference_offsets != expected_references) {
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
    if (constructor) {
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
          !types.contains(void_identity)) {
        issues.add(record, "constructor initializer signature is incomplete");
      } else {
        const ImportedAbiParameter& receiver =
            callable.initializer_parameters.front();
        if (receiver.kind != AbiParameterKind::kReceiver ||
            receiver.type_identity != file.identity) {
          issues.add(record,
                     "constructor initializer receiver ABI is inconsistent");
        }
        for (std::size_t index = 0; index < declaration.parameters.size();
             ++index) {
          const ImportedAbiParameter& parameter =
              callable.initializer_parameters[index + 1];
          if (parameter.kind != AbiParameterKind::kExplicit ||
              parameter.type_identity !=
                  declaration.parameters[index].type_identity) {
            issues.add(record,
                       "constructor initializer parameter ABI is inconsistent");
          }
        }
      }
    } else if (callable.initializer_identity ||
               !callable.initializer_mangled_name.empty() ||
               callable.initializer_return_type_identity ||
               !callable.initializer_parameters.empty()) {
      issues.add(record, "function carries constructor initializer ABI");
    }
    const bool has_receiver = !constructor && !declaration.is_static;
    const std::size_t expected_parameter_count =
        declaration.parameters.size() + (has_receiver ? 1U : 0U);
    if (callable.parameters.size() != expected_parameter_count) {
      issues.add(record, "callable ABI parameter count is inconsistent");
    }
    std::size_t explicit_index = 0;
    for (std::size_t index = 0; index < callable.parameters.size(); ++index) {
      const ImportedAbiParameter& parameter = callable.parameters[index];
      if (!types.contains(parameter.type_identity)) {
        issues.add(record, "callable ABI references an unknown type");
      }
      if (has_receiver && index == 0) {
        if (parameter.kind != AbiParameterKind::kReceiver ||
            parameter.type_identity != file.identity) {
          issues.add(record, "callable receiver ABI is inconsistent");
        }
        continue;
      }
      if (parameter.kind != AbiParameterKind::kExplicit ||
          explicit_index >= declaration.parameters.size() ||
          parameter.type_identity !=
              declaration.parameters[explicit_index].type_identity) {
        issues.add(record, "explicit parameter ABI is inconsistent");
      }
      ++explicit_index;
    }
    const std::string& expected_return =
        constructor ? file.identity : declaration.type_identity;
    if (explicit_index != declaration.parameters.size() ||
        callable.return_type_identity != expected_return ||
        !types.contains(callable.return_type_identity)) {
      issues.add(record, "callable ABI signature is inconsistent");
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
      if (element != types.end() && element->second->kind == TypeKind::kEnum) {
        issues.add("type", "nullable enum value types are not supported");
      }
    }
  }

  for (const ImportedFile& file : view.files) {
    const std::string record = "file " + file.logical_path;
    const NominalKind expected_nominal =
        file.kind == FileTypeKind::kEnum        ? NominalKind::kEnum
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
            (file.kind == FileTypeKind::kEnum        ? TypeKind::kEnum
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
    if (file.kind == FileTypeKind::kEnum) {
      const auto& descriptor = file.abi.descriptor;
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
          descriptor.kind != AbiHeapObjectKind::kFileClass ||
          !descriptor.mangled_name.empty() || descriptor.parent_identity ||
          descriptor.size != 0 || descriptor.alignment != 1 ||
          !descriptor.reference_offsets.empty() ||
          !descriptor.virtual_function_identities.empty() ||
          !descriptor.interfaces.empty() ||
          descriptor.identity !=
              canonical_member_identity(file.nominal_identity,
                                        CanonicalMemberKind::kDescriptor, "") ||
          descriptor.display_name !=
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
  return issues.take();
}

}  // namespace cloth
