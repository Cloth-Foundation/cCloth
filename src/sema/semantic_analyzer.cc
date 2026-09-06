// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/semantic_analyzer.h"

#include "cloth/identity/package_identity.h"
#include "cloth/lexer/literal.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/canonical_identity.h"
#include "cloth/sema/constant_evaluator.h"
#include "cloth/sema/field_initialization_analysis.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/scalar_constants.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

constexpr std::string_view kPreludeSourcePackage = "lang";

bool is_standard_library_prelude_package(std::string_view source_package) {
  return source_package == kPreludeSourcePackage ||
         source_package.starts_with("lang.");
}

bool is_primitive_parse_type(TypeKind kind) noexcept {
  return kind == TypeKind::kBool || kind == TypeKind::kChar ||
         kind == TypeKind::kByte || kind == TypeKind::kInt8 ||
         kind == TypeKind::kInt16 || kind == TypeKind::kInt32 ||
         kind == TypeKind::kInt64 || kind == TypeKind::kUint8 ||
         kind == TypeKind::kUint16 || kind == TypeKind::kUint32 ||
         kind == TypeKind::kUint64 || kind == TypeKind::kFloat32 ||
         kind == TypeKind::kFloat64;
}

bool is_builtin_type_expression(TypeKind kind) noexcept {
  return is_primitive_parse_type(kind) || kind == TypeKind::kString ||
         kind == TypeKind::kObject || kind == TypeKind::kVoid;
}

struct ScopeEntry {
  std::string_view name;
  SymbolId symbol;
};

struct Scope {
  std::optional<std::size_t> parent;
  std::vector<ScopeEntry> entries;
};

struct ExpressionState {
  TypeId type{0};
  ValueCategory category{ValueCategory::kInvalid};
  std::optional<SymbolId> symbol{};
  std::vector<SymbolId> candidates{};
  std::optional<TypeId> checked_type{};
  std::optional<FileId> interface_dispatch{};
  std::optional<IntegerMetaOperation> integer_meta_operation{};
  bool may_divide_by_zero{false};
};

struct ErrorEffectSource {
  std::optional<TypeId> direct_type;
  std::optional<SymbolId> callee;
  SourceRange range;
};

struct ShiftLiteral {
  bool is_negative;
  std::uint64_t magnitude;
};

using NonNullSet = std::vector<SymbolId>;

struct ConditionFacts {
  NonNullSet when_true;
  NonNullSet when_false;
};

struct TransferContext {
  bool is_loop;
  std::optional<NonNullSet> breaks{};
  std::optional<NonNullSet> continues{};
};

bool contains_symbol(const NonNullSet& symbols, SymbolId symbol) {
  return std::find(symbols.begin(), symbols.end(), symbol) != symbols.end();
}

void add_symbol(NonNullSet& symbols, SymbolId symbol) {
  if (!contains_symbol(symbols, symbol)) {
    symbols.push_back(symbol);
  }
}

NonNullSet union_symbols(NonNullSet left, const NonNullSet& right) {
  for (const SymbolId symbol : right) {
    add_symbol(left, symbol);
  }
  return left;
}

NonNullSet intersect_symbols(const NonNullSet& left, const NonNullSet& right) {
  NonNullSet result;
  for (const SymbolId symbol : left) {
    if (contains_symbol(right, symbol)) {
      result.push_back(symbol);
    }
  }
  return result;
}

enum class VisibleFileKind {
  kSamePackage,
  kExplicitImport,
  kWildcardImport,
};

struct VisibleFile {
  std::string name;
  FileId file;
  VisibleFileKind kind;
  SourceRange range;
};

char ascii_lower(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<char>(character + ('a' - 'A'));
  }
  return character;
}

bool ascii_case_equal(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) {
      return false;
    }
  }
  return true;
}

std::string qualified_file_name(std::string_view owning_package,
                                std::string_view source_package,
                                std::string_view type_name) {
  std::string result;
  if (!owning_package.empty()) {
    result += owning_package;
    result += '.';
  }
  if (!source_package.empty()) {
    result += source_package;
    result += '.';
  }
  result += type_name;
  return result;
}

}  // namespace

class SemanticAnalyzer {
 public:
  SemanticAnalyzer(std::span<const FileClassDecl* const> files,
                   DiagnosticEngine& diagnostics,
                   std::span<const ImportedPackageView> imported_packages)
      : files_(files),
        diagnostics_(diagnostics),
        imported_packages_(imported_packages) {}

  SemanticAnalysisResult run() {
    register_compiler_errors();
    register_file_classes();
    register_imported_packages();
    register_standard_library_prelude();
    register_imports();
    register_type_relationships();
    register_members();
    register_standard_library_bridges();
    register_primitive_parse_intrinsics();
    index_members();
    validate_struct_layout_cycles();
    validate_interface_contracts();
    validate_overrides();
    validate_interface_conformance();
    analyze_definitions();
    analyze_error_effects();
    return SemanticAnalysisResult{std::move(model_),
                                  !diagnostics_.has_errors()};
  }

 private:
  void register_compiler_errors() {
    const SourceRange range = point_range(SourceLocation{"<core>", 0, 1, 1});
    error_message_symbol_ =
        model_.add_symbol(SemanticSymbol{SymbolKind::kField,
                                         "Message",
                                         model_.string_type(),
                                         {},
                                         Visibility::kPublic,
                                         std::nullopt,
                                         range});
    model_.mutable_symbol(error_message_symbol_).is_final = true;

    error_default_constructor_ =
        model_.add_symbol(SemanticSymbol{SymbolKind::kConstructor,
                                         "Error",
                                         model_.error_root_type(),
                                         {},
                                         Visibility::kPublic,
                                         std::nullopt,
                                         range});
    error_message_constructor_ =
        model_.add_symbol(SemanticSymbol{SymbolKind::kConstructor,
                                         "Error",
                                         model_.error_root_type(),
                                         {model_.string_type()},
                                         Visibility::kPublic,
                                         std::nullopt,
                                         range});
    division_by_zero_constructor_ =
        model_.add_symbol(SemanticSymbol{SymbolKind::kConstructor,
                                         "DivisionByZero",
                                         model_.division_by_zero_type(),
                                         {},
                                         Visibility::kPublic,
                                         std::nullopt,
                                         range});
  }

  void register_file_classes() {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      const FileClassDecl& syntax = *files_[index];
      const FileId file_id{index};
      bool identity_valid = true;

      for (std::size_t previous = 0; previous < index; ++previous) {
        const FileClassDecl& previous_syntax = *files_[previous];
        if (!ascii_case_equal(syntax.qualified_name,
                              previous_syntax.qualified_name)) {
          continue;
        }
        diagnostics_.error(
            point_range(syntax.range.begin),
            "file class '" + syntax.qualified_name +
                "' collides with another source file by ASCII case");
        diagnostics_.note(
            point_range(previous_syntax.range.begin),
            "previous file class is '" + previous_syntax.qualified_name + "'");
        identity_valid = false;
        break;
      }

      const bool is_prelude_declaration =
          syntax.owning_package == kStandardLibraryPackageName &&
          is_standard_library_prelude_package(syntax.package_name);
      const std::optional<TypeId> existing_type = model_.find_type(syntax.name);
      if (!is_prelude_declaration && existing_type &&
          model_.type(*existing_type).kind != TypeKind::kFileClass &&
          model_.type(*existing_type).kind != TypeKind::kInterface &&
          model_.type(*existing_type).kind != TypeKind::kEnum &&
          model_.type(*existing_type).kind != TypeKind::kStruct) {
        diagnostics_.error(
            point_range(syntax.range.begin),
            "file class name '" + syntax.name + "' conflicts with a core type");
        identity_valid = false;
      }

      TypeId type = model_.error_type();
      if (identity_valid) {
        const TypeKind type_kind =
            syntax.kind == FileTypeKind::kStruct      ? TypeKind::kStruct
            : syntax.kind == FileTypeKind::kEnum      ? TypeKind::kEnum
            : syntax.kind == FileTypeKind::kInterface ? TypeKind::kInterface
            : syntax.kind == FileTypeKind::kError     ? TypeKind::kErrorClass
                                                      : TypeKind::kFileClass;
        type = model_.add_type(
            SemanticType{type_kind, syntax.qualified_name, file_id});
      }
      const SymbolId class_symbol = model_.add_symbol(SemanticSymbol{
          syntax.kind == FileTypeKind::kStruct      ? SymbolKind::kStruct
          : syntax.kind == FileTypeKind::kEnum      ? SymbolKind::kEnum
          : syntax.kind == FileTypeKind::kInterface ? SymbolKind::kInterface
          : syntax.kind == FileTypeKind::kError     ? SymbolKind::kError
                                                    : SymbolKind::kFileClass,
          syntax.qualified_name,
          type,
          {},
          syntax.visibility,
          file_id,
          syntax.range,
          identity_valid});
      const SymbolId self_symbol =
          model_.add_symbol(SemanticSymbol{SymbolKind::kSelf,
                                           "self",
                                           type,
                                           {},
                                           Visibility::kPrivate,
                                           file_id,
                                           point_range(syntax.range.begin),
                                           identity_valid});

      FileSemantics file{
          type,
          class_symbol,
          self_symbol,
          std::vector<SymbolId>(syntax.fields.size(), class_symbol),
          std::vector<SymbolId>(syntax.functions.size(), class_symbol),
          std::vector<SymbolId>(syntax.constructors.size(), class_symbol),
          std::vector<ExpressionSemantics>(
              syntax.storage.expressions().size(),
              ExpressionSemantics{model_.error_type()}),
          std::vector<std::optional<SymbolId>>(
              syntax.storage.statements().size()),
          syntax.is_valid && identity_valid};
      file.is_abstract = syntax.is_abstract;
      file.is_sealed = syntax.is_sealed;
      file.kind = syntax.kind;
      file.identity = NominalIdentity{
          PackageIdentity{syntax.owning_package, syntax.owning_package_version},
          syntax.package_name, syntax.name,
          syntax.kind == FileTypeKind::kStruct      ? NominalKind::kStruct
          : syntax.kind == FileTypeKind::kEnum      ? NominalKind::kEnum
          : syntax.kind == FileTypeKind::kInterface ? NominalKind::kInterface
                                                    : NominalKind::kClass};
      file.member_order = syntax.member_order;
      if (syntax.kind == FileTypeKind::kInterface) {
        file.interface_id = canonical_interface_id(file.identity);
      }
      if (syntax.is_abstract && syntax.is_sealed) {
        diagnostics_.error(syntax.range,
                           "file class '" + syntax.qualified_name +
                               "' cannot be both abstract and sealed");
        file.is_valid = false;
      }
      static_cast<void>(model_.add_file(std::move(file)));
      if (syntax.kind == FileTypeKind::kStruct && identity_valid) {
        model_.add_intrinsic("print", {type}, IntrinsicKind::kPrintStruct);
        model_.add_intrinsic("println", {type}, IntrinsicKind::kPrintStruct);
      }
      if (syntax.kind == FileTypeKind::kEnum && identity_valid) {
        register_enum_output(type);
        for (const EnumCaseDecl& enum_case : syntax.enum_cases) {
          register_enum_case(file_id, enum_case.name, enum_case.range);
        }
      }
    }
    visible_files_.resize(files_.size());
  }

  void register_imported_packages() {
    if (imported_packages_.empty()) {
      return;
    }

    auto imported_range = [](const ImportedSourceLocation& location) {
      return point_range(SourceLocation{
          location.path, 0, static_cast<std::uint32_t>(location.line),
          static_cast<std::uint32_t>(location.column)});
    };
    auto report_invalid = [this](std::string_view record) {
      diagnostics_.error(
          SourceLocation{"<artifact>", 0, 1, 1},
          "verified imported declaration could not be registered: " +
              std::string{record});
    };

    for (const ImportedPackageView& package : imported_packages_) {
      for (const ImportedFile& imported : package.files) {
        if (imported_file_ids_.contains(imported.identity)) {
          report_invalid(imported.identity);
          continue;
        }
        const FileId file_id{model_.files().size()};
        const TypeKind type_kind =
            imported.kind == FileTypeKind::kStruct      ? TypeKind::kStruct
            : imported.kind == FileTypeKind::kEnum      ? TypeKind::kEnum
            : imported.kind == FileTypeKind::kInterface ? TypeKind::kInterface
            : imported.kind == FileTypeKind::kError     ? TypeKind::kErrorClass
                                                        : TypeKind::kFileClass;
        const std::string qualified_name =
            qualified_file_name(imported.nominal_identity.package.name,
                                imported.nominal_identity.source_package,
                                imported.nominal_identity.name);
        const TypeId type =
            model_.add_type(SemanticType{type_kind, qualified_name, file_id});
        const SourceRange range = imported_range(imported.location);
        const SymbolId class_symbol = model_.add_symbol(SemanticSymbol{
            imported.kind == FileTypeKind::kStruct      ? SymbolKind::kStruct
            : imported.kind == FileTypeKind::kEnum      ? SymbolKind::kEnum
            : imported.kind == FileTypeKind::kInterface ? SymbolKind::kInterface
            : imported.kind == FileTypeKind::kError     ? SymbolKind::kError
                                                    : SymbolKind::kFileClass,
            qualified_name,
            type,
            {},
            imported.visibility,
            file_id,
            range});
        const SymbolId self_symbol =
            model_.add_symbol(SemanticSymbol{SymbolKind::kSelf,
                                             "self",
                                             type,
                                             {},
                                             Visibility::kPrivate,
                                             file_id,
                                             range});
        FileSemantics file{type, class_symbol, self_symbol, {}, {}, {}, {}, {}};
        file.is_abstract = imported.is_abstract;
        file.is_sealed = imported.is_sealed;
        file.kind = imported.kind;
        file.interface_id = imported.interface_id;
        file.identity = imported.nominal_identity;
        static_cast<void>(model_.add_file(std::move(file)));
        if (imported.kind == FileTypeKind::kStruct) {
          model_.add_intrinsic("print", {type}, IntrinsicKind::kPrintStruct);
          model_.add_intrinsic("println", {type}, IntrinsicKind::kPrintStruct);
        }
        if (imported.kind == FileTypeKind::kEnum) {
          register_enum_output(type);
          for (const ImportedEnumCase& enum_case : imported.enum_cases) {
            register_enum_case(file_id, enum_case.name,
                               imported_range(enum_case.location));
          }
        }
        imported_file_ids_.emplace(imported.identity, file_id);
        imported_type_ids_.emplace(imported.identity, type);
      }
    }

    for (std::size_t index = 0; index < model_.types().size(); ++index) {
      const TypeId type{index};
      imported_type_ids_.try_emplace(canonical_type_identity(type, model_),
                                     type);
    }
    std::size_t remaining_types = 0;
    for (const ImportedPackageView& package : imported_packages_) {
      for (const ImportedType& imported : package.types) {
        if (!imported_type_ids_.contains(imported.identity)) {
          ++remaining_types;
        }
      }
    }
    while (remaining_types != 0) {
      bool made_progress = false;
      for (const ImportedPackageView& package : imported_packages_) {
        for (const ImportedType& imported : package.types) {
          if (imported_type_ids_.contains(imported.identity)) {
            continue;
          }
          std::optional<TypeId> type;
          if ((imported.kind == TypeKind::kArray ||
               imported.kind == TypeKind::kNullable) &&
              imported.element_identity) {
            const auto element =
                imported_type_ids_.find(*imported.element_identity);
            if (element == imported_type_ids_.end()) {
              continue;
            }
            type = imported.kind == TypeKind::kArray
                       ? model_.get_array_type(element->second)
                       : model_.get_nullable_type(element->second);
          } else {
            for (std::size_t index = 0; index < model_.types().size();
                 ++index) {
              const TypeId candidate{index};
              if (model_.type(candidate).kind == imported.kind &&
                  canonical_type_identity(candidate, model_) ==
                      imported.identity) {
                type = candidate;
                break;
              }
            }
          }
          if (!type) {
            continue;
          }
          imported_type_ids_.emplace(imported.identity, *type);
          --remaining_types;
          made_progress = true;
        }
      }
      if (!made_progress) {
        report_invalid("type closure");
        break;
      }
    }

    for (const ImportedPackageView& package : imported_packages_) {
      for (const ImportedFile& imported_file : package.files) {
        const auto file_entry = imported_file_ids_.find(imported_file.identity);
        if (file_entry == imported_file_ids_.end()) {
          continue;
        }
        const FileId file_id = file_entry->second;
        for (const ImportedMember& imported : imported_file.members) {
          const auto type = imported_type_ids_.find(imported.type_identity);
          if (type == imported_type_ids_.end() ||
              imported_member_ids_.contains(imported.identity)) {
            report_invalid(imported.identity);
            continue;
          }
          std::vector<TypeId> parameter_types;
          if (imported.static_value &&
              (imported.static_value->kind != model_.type(type->second).kind ||
               !is_valid_scalar_constant(
                   {type->second, imported.static_value->bits}, type->second,
                   model_))) {
            report_invalid(imported.identity);
            continue;
          }
          std::vector<SymbolId> parameter_symbols;
          bool parameters_valid = true;
          for (const ImportedParameter& parameter : imported.parameters) {
            const auto parameter_type =
                imported_type_ids_.find(parameter.type_identity);
            if (parameter_type == imported_type_ids_.end()) {
              parameters_valid = false;
              break;
            }
            parameter_types.push_back(parameter_type->second);
            const SourceRange range = imported_range(imported.location);
            const SymbolId parameter_symbol =
                model_.add_symbol(SemanticSymbol{SymbolKind::kParameter,
                                                 parameter.name,
                                                 parameter_type->second,
                                                 {},
                                                 Visibility::kPrivate,
                                                 file_id,
                                                 range});
            model_.mutable_symbol(parameter_symbol).is_final =
                parameter.is_final;
            parameter_symbols.push_back(parameter_symbol);
          }
          if (!parameters_valid) {
            report_invalid(imported.identity);
            continue;
          }
          std::vector<TypeId> thrown_types;
          bool thrown_types_valid = true;
          for (const std::string& identity : imported.thrown_type_identities) {
            const auto thrown_type = imported_type_ids_.find(identity);
            if (thrown_type == imported_type_ids_.end()) {
              thrown_types_valid = false;
              break;
            }
            thrown_types.push_back(thrown_type->second);
          }
          if (!thrown_types_valid) {
            report_invalid(imported.identity);
            continue;
          }
          SymbolKind kind = SymbolKind::kField;
          if (imported.kind == ImportedMemberKind::kFunction) {
            kind = SymbolKind::kFunction;
          } else if (imported.kind == ImportedMemberKind::kConstructor) {
            kind = SymbolKind::kConstructor;
          }
          const SymbolId symbol = model_.add_symbol(SemanticSymbol{
              kind, imported.name, type->second, std::move(parameter_types),
              imported.visibility, file_id, imported_range(imported.location)});
          SemanticSymbol& semantic = model_.mutable_symbol(symbol);
          semantic.parameter_symbols = std::move(parameter_symbols);
          semantic.is_final = imported.is_final;
          semantic.is_static = imported.is_static;
          semantic.is_override = imported.is_override;
          semantic.is_abstract = imported.is_abstract;
          semantic.virtual_slot = imported.virtual_slot;
          semantic.thrown_types = std::move(thrown_types);
          semantic.has_explicit_throws =
              kind == SymbolKind::kFunction || kind == SymbolKind::kConstructor;
          if (imported.static_value && semantic.is_static &&
              semantic.is_final) {
            semantic.static_constant =
                ScalarConstant{semantic.type, imported.static_value->bits};
          }
          imported_member_ids_.emplace(imported.identity, symbol);
        }
      }
    }

    auto file_id =
        [this](const std::string& identity) -> std::optional<FileId> {
      const auto found = imported_file_ids_.find(identity);
      return found == imported_file_ids_.end()
                 ? std::nullopt
                 : std::optional<FileId>{found->second};
    };
    auto member_id =
        [this](const std::string& identity) -> std::optional<SymbolId> {
      const auto found = imported_member_ids_.find(identity);
      return found == imported_member_ids_.end()
                 ? std::nullopt
                 : std::optional<SymbolId>{found->second};
    };
    for (const ImportedPackageView& package : imported_packages_) {
      for (const ImportedFile& imported : package.files) {
        const auto current = file_id(imported.identity);
        if (!current) {
          continue;
        }
        FileSemantics& semantic_file = model_.mutable_file(*current);
        if (imported.base_identity) {
          semantic_file.base_file = file_id(*imported.base_identity);
        }
        for (const std::string& identity :
             imported.direct_interface_identities) {
          if (const auto interface = file_id(identity)) {
            semantic_file.direct_interfaces.push_back(*interface);
          }
        }
        for (const std::string& identity : imported.interface_identities) {
          if (const auto interface = file_id(identity)) {
            semantic_file.interfaces.push_back(*interface);
          }
        }

        std::map<std::string, const ImportedMember*, std::less<>> members;
        for (const ImportedMember& member : imported.members) {
          members.emplace(member.identity, &member);
          const auto symbol = member_id(member.identity);
          if (!symbol) {
            continue;
          }
          SemanticSymbol& semantic = model_.mutable_symbol(*symbol);
          if (member.overridden_identity) {
            semantic.overridden_symbol = member_id(*member.overridden_identity);
          }
          if (member.base_constructor_identity) {
            semantic.base_constructor =
                member_id(*member.base_constructor_identity);
          }
        }
        for (const std::string& identity : imported.member_order) {
          const auto member = members.find(identity);
          const auto symbol = member_id(identity);
          if (member == members.end() || !symbol) {
            report_invalid(identity);
            continue;
          }
          if (member->second->kind == ImportedMemberKind::kField) {
            semantic_file.member_order.push_back(MemberReference{
                DeclarationKind::kField, semantic_file.fields.size()});
            semantic_file.fields.push_back(*symbol);
          } else if (member->second->kind == ImportedMemberKind::kFunction) {
            semantic_file.member_order.push_back(MemberReference{
                DeclarationKind::kFunction, semantic_file.functions.size()});
            semantic_file.functions.push_back(*symbol);
          } else {
            semantic_file.member_order.push_back(
                MemberReference{DeclarationKind::kConstructor,
                                semantic_file.constructors.size()});
            semantic_file.constructors.push_back(*symbol);
          }
        }
        for (const std::string& identity :
             imported.virtual_function_identities) {
          if (const auto function = member_id(identity)) {
            semantic_file.virtual_functions.push_back(*function);
          }
        }
        for (const std::string& identity :
             imported.abstract_function_identities) {
          if (const auto function = member_id(identity)) {
            semantic_file.abstract_functions.push_back(*function);
          }
        }
        for (const std::string& identity :
             imported.interface_function_identities) {
          if (const auto function = member_id(identity)) {
            semantic_file.interface_functions.push_back(*function);
          }
        }
        for (const ImportedInterfaceImplementation& implementation :
             imported.interface_implementations) {
          const auto interface = file_id(implementation.interface_identity);
          if (!interface) {
            report_invalid(implementation.interface_identity);
            continue;
          }
          std::vector<SymbolId> functions;
          for (const std::string& identity :
               implementation.function_identities) {
            if (const auto function = member_id(identity)) {
              functions.push_back(*function);
            }
          }
          semantic_file.interface_implementations.push_back(
              InterfaceImplementation{*interface, std::move(functions)});
        }
      }
    }
  }

  void register_imports() {
    for (std::size_t current_index = 0; current_index < files_.size();
         ++current_index) {
      const FileId current_file{current_index};
      const FileClassDecl& current = *files_[current_index];
      for (std::size_t target_index = 0; target_index < model_.files().size();
           ++target_index) {
        const FileSemantics& target = model_.file(FileId{target_index});
        if (target.identity.package.name != current.owning_package ||
            target.identity.source_package != current.package_name) {
          continue;
        }
        const FileId target_file{target_index};
        const SemanticSymbol& symbol =
            model_.symbol(model_.file(target_file).symbol);
        if (target_file != current_file &&
            symbol.visibility == Visibility::kPrivate) {
          continue;
        }
        visible_files_[current_index].push_back(VisibleFile{
            target.identity.name, target_file, VisibleFileKind::kSamePackage,
            model_.symbol(target.symbol).range});
      }

      for (const ImportDecl& import : current.imports) {
        if (import.is_valid && import.kind == ImportKind::kType) {
          register_explicit_import(current_file, import);
        }
      }
      for (const ImportDecl& import : current.imports) {
        if (import.is_valid && import.kind == ImportKind::kWildcard) {
          register_wildcard_import(current_file, import);
        }
      }
    }
  }

  void register_standard_library_prelude() {
    std::vector<FileId> candidates;
    for (std::size_t index = 0; index < model_.files().size(); ++index) {
      const FileId file_id{index};
      const FileSemantics& file = model_.file(file_id);
      const SemanticSymbol& symbol = model_.symbol(file.symbol);
      if (file.identity.package.name == kStandardLibraryPackageName &&
          is_standard_library_prelude_package(file.identity.source_package) &&
          symbol.visibility == Visibility::kPublic && file.is_valid &&
          file.type != model_.error_type()) {
        candidates.push_back(file_id);
      }
    }
    std::ranges::sort(candidates, {}, [this](FileId file_id) {
      return model_.symbol(model_.file(file_id).symbol).name;
    });

    std::map<std::string, std::vector<FileId>, std::less<>>
        candidates_by_short_name;
    for (const FileId file_id : candidates) {
      candidates_by_short_name[model_.file(file_id).identity.name].push_back(
          file_id);
    }

    for (const auto& [name, same_named_files] : candidates_by_short_name) {
      if (const std::optional<TypeId> core = find_core_type(name)) {
        for (const FileId file_id : same_named_files) {
          const SemanticSymbol& symbol =
              model_.symbol(model_.file(file_id).symbol);
          diagnostics_.error(symbol.range, "standard-library prelude type '" +
                                               symbol.name +
                                               "' conflicts with core type '" +
                                               model_.type(*core).name + "'");
          model_.mutable_file(file_id).is_valid = false;
        }
        continue;
      }
      if (same_named_files.size() != 1) {
        std::string identities;
        for (const FileId file_id : same_named_files) {
          if (!identities.empty()) {
            identities += ", ";
          }
          identities +=
              "'" + model_.symbol(model_.file(file_id).symbol).name + "'";
          model_.mutable_file(file_id).is_valid = false;
        }
        const SemanticSymbol& first =
            model_.symbol(model_.file(same_named_files.front()).symbol);
        diagnostics_.error(first.range, "standard-library prelude name '" +
                                            name + "' is ambiguous between " +
                                            identities);
        continue;
      }
      prelude_files_.emplace(name, same_named_files.front());
    }
  }

  void register_explicit_import(FileId current_file, const ImportDecl& import) {
    const std::string target_name = qualified_file_name(
        import.target_package, import.package_name, import.type_name);
    const std::optional<FileId> target = find_qualified_file(target_name);
    if (!target) {
      diagnostics_.error(import.range,
                         "unknown imported file class '" + target_name + "'");
      model_.mutable_file(current_file).is_valid = false;
      return;
    }
    const SemanticSymbol& symbol = model_.symbol(model_.file(*target).symbol);
    if (*target != current_file && symbol.visibility == Visibility::kPrivate) {
      diagnostics_.error(import.range,
                         "file class '" + target_name + "' is private");
      model_.mutable_file(current_file).is_valid = false;
      return;
    }
    bind_visible_file(current_file, import.local_name, *target,
                      VisibleFileKind::kExplicitImport, import.range);
  }

  void register_wildcard_import(FileId current_file, const ImportDecl& import) {
    bool found_package = false;
    for (std::size_t index = 0; index < model_.files().size(); ++index) {
      const FileSemantics& target = model_.file(FileId{index});
      if (target.identity.package.name != import.target_package ||
          target.identity.source_package != import.package_name) {
        continue;
      }
      found_package = true;
      const FileId target_file{index};
      const SemanticSymbol& symbol =
          model_.symbol(model_.file(target_file).symbol);
      if (symbol.visibility == Visibility::kPrivate) {
        continue;
      }
      bind_visible_file(current_file, target.identity.name, target_file,
                        VisibleFileKind::kWildcardImport, import.range);
    }
    if (!found_package) {
      diagnostics_.error(import.range,
                         "unknown package '" + import.package_name + "'");
      model_.mutable_file(current_file).is_valid = false;
    }
  }

  void bind_visible_file(FileId current_file, std::string_view name,
                         FileId target, VisibleFileKind kind,
                         SourceRange range) {
    if (find_core_type(name)) {
      diagnostics_.error(range, "import name '" + std::string{name} +
                                    "' conflicts with a core type");
      model_.mutable_file(current_file).is_valid = false;
      return;
    }

    std::vector<VisibleFile>& bindings = visible_files_[current_file.value];
    for (const VisibleFile& binding : bindings) {
      if (binding.name != name) {
        continue;
      }
      if (binding.file == target) {
        return;
      }
      if (kind == VisibleFileKind::kWildcardImport &&
          binding.kind != VisibleFileKind::kWildcardImport) {
        return;
      }
      diagnostics_.error(
          range, "import name '" + std::string{name} + "' is ambiguous");
      diagnostics_.note(binding.range, "previous binding is here");
      model_.mutable_file(current_file).is_valid = false;
      return;
    }
    bindings.push_back(VisibleFile{std::string{name}, target, kind, range});
  }

  std::optional<TypeId> find_core_type(std::string_view name) const {
    const std::optional<TypeId> type = model_.find_type(name);
    if (!type) {
      return std::nullopt;
    }
    const TypeKind kind = model_.type(*type).kind;
    if (kind != TypeKind::kFileClass && kind != TypeKind::kInterface &&
        kind != TypeKind::kEnum && kind != TypeKind::kStruct &&
        kind != TypeKind::kErrorClass) {
      return type;
    }
    if (*type == model_.error_root_type() ||
        *type == model_.division_by_zero_type()) {
      return type;
    }
    return std::nullopt;
  }

  void register_type_relationships() {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      const FileId file_id{index};
      const FileClassDecl& syntax = *files_[index];
      if (syntax.base_class) {
        const TypeId base_type = resolve_type(*syntax.base_class, file_id);
        if (base_type != model_.error_type()) {
          const SemanticType& base = model_.type(base_type);
          if (syntax.kind == FileTypeKind::kError) {
            if (base.kind != TypeKind::kErrorClass) {
              diagnostics_.error(
                  syntax.base_class->range,
                  "base type '" + base.name + "' must be an error");
              model_.mutable_file(file_id).is_valid = false;
            } else if (base_type == model_.division_by_zero_type()) {
              diagnostics_.error(
                  syntax.base_class->range,
                  "error '" + syntax.qualified_name +
                      "' cannot inherit from sealed error 'DivisionByZero'");
              model_.mutable_file(file_id).is_valid = false;
            } else if (base.file && *base.file == file_id) {
              diagnostics_.error(syntax.base_class->range,
                                 "error '" + syntax.qualified_name +
                                     "' cannot inherit from itself");
              model_.mutable_file(file_id).is_valid = false;
            } else if (base.file && model_.file(*base.file).is_sealed) {
              diagnostics_.error(syntax.base_class->range,
                                 "error '" + syntax.qualified_name +
                                     "' cannot inherit from sealed error '" +
                                     base.name + "'");
              diagnostics_.note(
                  model_.symbol(model_.file(*base.file).symbol).range,
                  "sealed error is declared here");
              model_.mutable_file(file_id).is_valid = false;
            } else if (base.file) {
              model_.mutable_file(file_id).base_file = *base.file;
            }
          } else if (syntax.kind != FileTypeKind::kClass) {
            diagnostics_.error(syntax.base_class->range,
                               "only classes can inherit a class");
            model_.mutable_file(file_id).is_valid = false;
          } else if (base.kind != TypeKind::kFileClass || !base.file) {
            diagnostics_.error(
                syntax.base_class->range,
                "base type '" + base.name + "' must be a file class");
            model_.mutable_file(file_id).is_valid = false;
          } else if (*base.file == file_id) {
            diagnostics_.error(syntax.base_class->range,
                               "file class '" + syntax.qualified_name +
                                   "' cannot inherit from itself");
            model_.mutable_file(file_id).is_valid = false;
          } else if (model_.file(*base.file).is_sealed) {
            diagnostics_.error(syntax.base_class->range,
                               "file class '" + syntax.qualified_name +
                                   "' cannot inherit from sealed file class '" +
                                   base.name + "'");
            diagnostics_.note(
                model_.symbol(model_.file(*base.file).symbol).range,
                "sealed file class is declared here");
            model_.mutable_file(file_id).is_valid = false;
          } else {
            model_.mutable_file(file_id).base_file = *base.file;
          }
        }
      }

      for (const TypeSyntax& interface_syntax : syntax.interfaces) {
        if (syntax.kind == FileTypeKind::kStruct) {
          diagnostics_.error(interface_syntax.range,
                             "structs cannot implement interfaces");
          model_.mutable_file(file_id).is_valid = false;
          continue;
        }
        const TypeId type = resolve_type(interface_syntax, file_id);
        if (type == model_.error_type()) {
          continue;
        }
        const SemanticType& interface_type = model_.type(type);
        if (interface_type.kind != TypeKind::kInterface ||
            !interface_type.file) {
          diagnostics_.error(
              interface_syntax.range,
              "type '" + interface_type.name + "' is not an interface");
          model_.mutable_file(file_id).is_valid = false;
          continue;
        }
        if (*interface_type.file == file_id) {
          diagnostics_.error(interface_syntax.range,
                             "interface '" + syntax.qualified_name +
                                 "' cannot inherit from itself");
          model_.mutable_file(file_id).is_valid = false;
          continue;
        }
        std::vector<FileId>& direct =
            model_.mutable_file(file_id).direct_interfaces;
        if (std::ranges::find(direct, *interface_type.file) != direct.end()) {
          diagnostics_.error(interface_syntax.range, "duplicate interface '" +
                                                         interface_type.name +
                                                         "' in declaration");
          model_.mutable_file(file_id).is_valid = false;
          continue;
        }
        direct.push_back(*interface_type.file);
      }
    }

    for (std::size_t left = 0; left < model_.files().size(); ++left) {
      const FileSemantics& left_file = model_.file(FileId{left});
      if (!left_file.interface_id) {
        continue;
      }
      for (std::size_t right = 0; right < left; ++right) {
        const FileSemantics& right_file = model_.file(FileId{right});
        if (right_file.interface_id == left_file.interface_id) {
          diagnostics_.error(model_.symbol(left_file.symbol).range,
                             "interface runtime identity collides with '" +
                                 model_.symbol(right_file.symbol).name + "'");
          model_.mutable_file(FileId{left}).is_valid = false;
          model_.mutable_file(FileId{right}).is_valid = false;
        }
      }
    }
    validate_inheritance_cycles();
  }

  void validate_inheritance_cycles() {
    enum class VisitState {
      kUnvisited,
      kVisiting,
      kComplete,
    };

    std::vector<VisitState> states(files_.size(), VisitState::kUnvisited);
    std::vector<bool> hierarchy_invalid(files_.size(), false);
    std::vector<FileId> path;
    std::vector<FileId> order;
    order.reserve(files_.size());
    for (std::size_t index = 0; index < files_.size(); ++index) {
      order.push_back(FileId{index});
    }
    std::ranges::sort(order, {}, [this](FileId file) {
      return files_[file.value]->qualified_name;
    });

    for (const FileId root : order) {
      if (states[root.value] != VisitState::kUnvisited) {
        continue;
      }
      path.clear();
      std::optional<FileId> current = root;
      while (current && current->value < files_.size() &&
             states[current->value] == VisitState::kUnvisited) {
        states[current->value] = VisitState::kVisiting;
        path.push_back(*current);
        current = model_.file(*current).base_file;
      }

      if (current && current->value < files_.size() &&
          states[current->value] == VisitState::kVisiting) {
        const auto cycle_begin = std::find(path.begin(), path.end(), *current);
        std::string message = "inheritance cycle detected: ";
        for (auto member = cycle_begin; member != path.end(); ++member) {
          if (member != cycle_begin) {
            message += " -> ";
          }
          message += model_.symbol(model_.file(*member).symbol).name;
          hierarchy_invalid[member->value] = true;
        }
        message += " -> " + model_.symbol(model_.file(*current).symbol).name;
        const FileClassDecl& syntax = *files_[path.back().value];
        diagnostics_.error(syntax.base_class->range, std::move(message));
      }

      for (auto member = path.rbegin(); member != path.rend(); ++member) {
        const std::optional<FileId> base = model_.file(*member).base_file;
        if (base && base->value < hierarchy_invalid.size() &&
            hierarchy_invalid[base->value]) {
          hierarchy_invalid[member->value] = true;
        }
        states[member->value] = VisitState::kComplete;
      }
    }
    for (std::size_t index = 0; index < hierarchy_invalid.size(); ++index) {
      if (hierarchy_invalid[index]) {
        FileSemantics& file = model_.mutable_file(FileId{index});
        file.is_valid = false;
        file.base_file.reset();
      }
    }
  }

  void register_enum_output(TypeId type) {
    model_.add_intrinsic("print", {type}, IntrinsicKind::kPrintEnum);
    model_.add_intrinsic("println", {type}, IntrinsicKind::kPrintEnum);
  }

  void register_enum_case(FileId file, std::string_view name,
                          SourceRange range) {
    const TypeId type = model_.file(file).type;
    const auto tag =
        static_cast<std::uint32_t>(model_.file(file).enum_cases.size());
    SemanticSymbol symbol{SymbolKind::kEnumCase,
                          std::string{name},
                          type,
                          {},
                          Visibility::kPublic,
                          file,
                          range};
    symbol.enum_tag = tag;
    symbol.is_final = true;
    symbol.is_static = true;
    const SymbolId id = model_.add_symbol(std::move(symbol));
    model_.mutable_file(file).enum_cases.push_back(id);
    enum_case_names_[file.value].emplace(name, id);
  }

  void register_members() {
    for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
      const FileId file_id{file_index};
      const FileClassDecl& syntax = *files_[file_index];
      for (const MemberReference& reference : syntax.member_order) {
        switch (reference.kind) {
          case DeclarationKind::kField:
            register_field(file_id, reference.index);
            break;
          case DeclarationKind::kFunction:
            register_function(file_id, reference.index);
            break;
          case DeclarationKind::kConstructor:
            register_constructor(file_id, reference.index);
            break;
          case DeclarationKind::kNestedType:
            break;
        }
      }
    }
  }

  void register_standard_library_bridges() {
    std::optional<FileId> console_file;
    std::optional<TypeId> io_error;
    for (std::size_t index = 0; index < files_.size(); ++index) {
      const FileSemantics& file = model_.file(FileId{index});
      const NominalIdentity& identity = file.identity;
      if (identity.package.name != kStandardLibraryPackageName ||
          identity.package.version != kStandardLibraryPackageVersion) {
        continue;
      }
      if (identity.source_package == "io" && identity.name == "Console" &&
          file.kind == FileTypeKind::kClass) {
        console_file = FileId{index};
      } else if (identity.source_package == "lang.errors" &&
                 identity.name == "IoError" &&
                 file.kind == FileTypeKind::kError) {
        io_error = file.type;
      }
    }
    if (!console_file || !io_error) {
      return;
    }

    SemanticSymbol bridge{
        SymbolKind::kFunction,
        "__readLine",
        model_.get_nullable_type(model_.string_type()),
        {},
        Visibility::kPrivate,
        std::nullopt,
        model_.symbol(model_.file(*console_file).symbol).range};
    bridge.intrinsic = IntrinsicKind::kConsoleReadLine;
    bridge.is_static = true;
    bridge.thrown_types = std::vector<TypeId>{*io_error};
    bridge.has_explicit_throws = true;
    static_cast<void>(model_.add_symbol(std::move(bridge)));
  }

  void register_primitive_parse_intrinsics() {
    std::optional<TypeId> parse_error;
    SourceRange bridge_range = point_range(SourceLocation{"<core>", 0, 1, 1});
    for (std::size_t index = 0; index < model_.files().size(); ++index) {
      const FileSemantics& file = model_.file(FileId{index});
      const NominalIdentity& identity = file.identity;
      if (identity.package.name != kStandardLibraryPackageName ||
          identity.package.version != kStandardLibraryPackageVersion ||
          identity.source_package != "lang.errors" ||
          identity.name != "ParseError" || file.kind != FileTypeKind::kError) {
        continue;
      }
      const bool has_message_constructor =
          std::ranges::any_of(file.constructors, [&](SymbolId candidate) {
            const SemanticSymbol& constructor = model_.symbol(candidate);
            return constructor.is_valid &&
                   constructor.visibility == Visibility::kPublic &&
                   constructor.parameter_types.size() == 1 &&
                   constructor.parameter_types.front() == model_.string_type();
          });
      if (has_message_constructor) {
        parse_error = file.type;
        bridge_range = model_.symbol(file.symbol).range;
      }
      break;
    }
    if (!parse_error) {
      return;
    }

    const std::size_t type_count = model_.types().size();
    for (std::size_t index = 0; index < type_count; ++index) {
      const TypeId type{index};
      if (!is_primitive_parse_type(model_.type(type).kind)) {
        continue;
      }
      SemanticSymbol bridge{SymbolKind::kFunction,
                            "__primitiveParse",
                            type,
                            {model_.string_type()},
                            Visibility::kPrivate,
                            std::nullopt,
                            bridge_range};
      bridge.intrinsic = IntrinsicKind::kPrimitiveParse;
      bridge.is_static = true;
      bridge.thrown_types = std::vector<TypeId>{*parse_error};
      bridge.has_explicit_throws = true;
      static_cast<void>(model_.add_symbol(std::move(bridge)));
    }
  }

  bool is_standard_library_console(FileId file_id) const {
    if (file_id.value >= files_.size()) {
      return false;
    }
    const FileSemantics& file = model_.file(file_id);
    const NominalIdentity& identity = file.identity;
    return identity.package.name == kStandardLibraryPackageName &&
           identity.package.version == kStandardLibraryPackageVersion &&
           identity.source_package == "io" && identity.name == "Console" &&
           file.kind == FileTypeKind::kClass;
  }

  // Only inline struct fields add layout dependencies. Iterative DFS keeps
  // source-controlled nesting off the compiler's native call stack.
  void validate_struct_layout_cycles() {
    struct Frame {
      FileId file;
      std::size_t next_field{0};
      std::optional<SymbolId> incoming{};
    };
    std::vector<unsigned char> state(model_.files().size(), 0);
    std::vector<Frame> path;
    for (std::size_t index = 0; index < model_.files().size(); ++index) {
      if (state[index] != 0 ||
          model_.file(FileId{index}).kind != FileTypeKind::kStruct) {
        continue;
      }
      path.push_back(Frame{FileId{index}});
      state[index] = 1;
      while (!path.empty()) {
        Frame& frame = path.back();
        const FileSemantics& file = model_.file(frame.file);
        if (frame.next_field == file.fields.size()) {
          state[frame.file.value] = 2;
          path.pop_back();
          continue;
        }
        const SymbolId field_id = file.fields[frame.next_field++];
        const SemanticSymbol& field = model_.symbol(field_id);
        const SemanticType& type = model_.type(field.type);
        if (field.is_static || type.kind != TypeKind::kStruct || !type.file) {
          continue;
        }
        if (state[type.file->value] == 0) {
          state[type.file->value] = 1;
          path.push_back(Frame{*type.file, 0, field_id});
        } else if (state[type.file->value] == 1) {
          const auto start = std::ranges::find(path, *type.file, &Frame::file);
          std::string cycle;
          for (auto current = start; current != path.end(); ++current) {
            const SymbolId edge =
                current + 1 == path.end() ? field_id : *(current + 1)->incoming;
            const SemanticSymbol& member = model_.symbol(edge);
            cycle += model_.type(model_.file(current->file).type).name + "." +
                     member.name + " -> ";
            model_.mutable_file(current->file).is_valid = false;
          }
          cycle += type.name;
          diagnostics_.error(field.range,
                             "inline struct layout cycle: " + cycle);
          for (auto current = start + 1; current != path.end(); ++current) {
            diagnostics_.note(model_.symbol(*current->incoming).range,
                              "embedded field participates in this cycle");
          }
        }
      }
    }
  }

  void register_field(FileId file_id, std::size_t index) {
    const FieldDecl& field = files_[file_id.value]->fields.at(index);
    const TypeId type = resolve_type(field.type, file_id);
    bool valid = field.is_valid && type != model_.error_type();
    if (model_.file(file_id).kind == FileTypeKind::kError &&
        field.name == "Message") {
      diagnostics_.error(field.range,
                         "error field 'Message' cannot hide inherited final "
                         "field 'Error.Message'");
      diagnostics_.note(model_.symbol(error_message_symbol_).range,
                        "compiler-provided final field is declared here");
      model_.mutable_file(file_id).is_valid = false;
      valid = false;
    }
    const SymbolId symbol =
        model_.add_symbol(SemanticSymbol{SymbolKind::kField,
                                         std::string{field.name},
                                         type,
                                         {},
                                         field.visibility,
                                         file_id,
                                         field.range,
                                         valid});
    model_.mutable_symbol(symbol).is_final = field.is_final;
    model_.mutable_symbol(symbol).is_static = field.is_static;
    model_.mutable_file(file_id).fields.at(index) = symbol;
  }

  std::vector<TypeId> resolve_throws_types(std::span<const TypeSyntax> syntaxes,
                                           FileId file_id,
                                           Visibility callable_visibility) {
    std::vector<TypeId> types;
    std::vector<SourceRange> type_ranges;
    types.reserve(syntaxes.size());
    for (const TypeSyntax& syntax : syntaxes) {
      const TypeId type = resolve_type(syntax, file_id);
      if (type == model_.error_type()) {
        continue;
      }
      if (!is_error_type(type)) {
        diagnostics_.error(syntax.range, "throws clause type '" +
                                             type_name(type) +
                                             "' is not a non-null error");
        model_.mutable_file(file_id).is_valid = false;
        continue;
      }
      if (callable_visibility == Visibility::kPublic) {
        const SemanticType& value = model_.type(type);
        if (value.file &&
            model_.symbol(model_.file(*value.file).symbol).visibility ==
                Visibility::kPrivate) {
          diagnostics_.error(syntax.range,
                             "public throws clause exposes private error '" +
                                 type_name(type) + "'");
          model_.mutable_file(file_id).is_valid = false;
          continue;
        }
      }
      if (std::ranges::find(types, type) != types.end()) {
        diagnostics_.error(
            syntax.range,
            "duplicate error type '" + type_name(type) + "' in throws clause");
        model_.mutable_file(file_id).is_valid = false;
        continue;
      }
      types.push_back(type);
      type_ranges.push_back(syntax.range);
    }

    for (std::size_t index = 0; index < types.size(); ++index) {
      for (std::size_t other = index + 1; other < types.size(); ++other) {
        std::optional<std::size_t> redundant;
        std::optional<std::size_t> covering;
        if (is_error_subtype(types[index], types[other])) {
          redundant = index;
          covering = other;
        } else if (is_error_subtype(types[other], types[index])) {
          redundant = other;
          covering = index;
        }
        if (!redundant) {
          continue;
        }
        diagnostics_.error(type_ranges[*redundant],
                           "error type '" + type_name(types[*redundant]) +
                               "' is already covered by '" +
                               type_name(types[*covering]) +
                               "' in throws clause");
        model_.mutable_file(file_id).is_valid = false;
      }
    }
    std::ranges::sort(types, [this](TypeId left, TypeId right) {
      return canonical_type_identity(left, model_) <
             canonical_type_identity(right, model_);
    });
    return types;
  }

  void register_function(FileId file_id, std::size_t index) {
    const FunctionDecl& function = files_[file_id.value]->functions.at(index);
    std::vector<TypeId> parameters;
    parameters.reserve(function.parameters.size());
    bool valid = function.is_valid;
    for (const ParameterDecl& parameter : function.parameters) {
      const TypeId type = resolve_type(parameter.type, file_id);
      parameters.push_back(type);
      valid = valid && type != model_.error_type();
    }
    TypeId return_type = model_.void_type();
    if (function.return_type) {
      return_type = resolve_type(*function.return_type, file_id, true);
      valid = valid && return_type != model_.error_type();
    }
    const SymbolId symbol = model_.add_symbol(
        SemanticSymbol{SymbolKind::kFunction, std::string{function.name},
                       return_type, std::move(parameters), function.visibility,
                       file_id, function.range, valid});
    model_.mutable_symbol(symbol).is_static = function.is_static;
    model_.mutable_symbol(symbol).is_override = function.is_override;
    model_.mutable_symbol(symbol).is_abstract = function.is_abstract;
    model_.mutable_symbol(symbol).is_final = function.is_final;
    model_.mutable_symbol(symbol).has_explicit_throws =
        function.has_explicit_throws;
    const std::size_t throws_diagnostic_begin =
        diagnostics_.diagnostics().size();
    model_.mutable_symbol(symbol).thrown_types = resolve_throws_types(
        function.throws_types, file_id, function.visibility);
    if (diagnostics_.diagnostics().size() != throws_diagnostic_begin) {
      model_.mutable_symbol(symbol).is_valid = false;
    }
    if (files_[file_id.value]->kind == FileTypeKind::kStruct &&
        (function.is_abstract || function.is_override || function.is_final)) {
      diagnostics_.error(
          function.range,
          "struct functions cannot be abstract, override, or final");
      model_.mutable_symbol(symbol).is_valid = false;
      model_.mutable_file(file_id).is_valid = false;
    }
    if (function.is_final && !function.is_override) {
      diagnostics_.error(function.range,
                         "final function '" + std::string{function.name} +
                             "' must also be declared override");
      model_.mutable_symbol(symbol).is_valid = false;
      model_.mutable_file(file_id).is_valid = false;
    }
    if (function.is_abstract) {
      if ((files_[file_id.value]->kind == FileTypeKind::kClass ||
           files_[file_id.value]->kind == FileTypeKind::kError) &&
          !files_[file_id.value]->is_abstract) {
        diagnostics_.error(function.range,
                           "abstract function '" + std::string{function.name} +
                               "' requires an abstract file class");
        model_.mutable_symbol(symbol).is_valid = false;
        model_.mutable_file(file_id).is_valid = false;
      }
      if (function.is_static) {
        diagnostics_.error(function.range, "abstract function '" +
                                               std::string{function.name} +
                                               "' cannot be static");
        model_.mutable_symbol(symbol).is_valid = false;
        model_.mutable_file(file_id).is_valid = false;
      }
      if (function.visibility == Visibility::kPrivate) {
        diagnostics_.error(function.range, "abstract function '" +
                                               std::string{function.name} +
                                               "' must be public");
        model_.mutable_symbol(symbol).is_valid = false;
        model_.mutable_file(file_id).is_valid = false;
      }
      if (function.is_final) {
        diagnostics_.error(function.range, "abstract function '" +
                                               std::string{function.name} +
                                               "' cannot be final");
        model_.mutable_symbol(symbol).is_valid = false;
        model_.mutable_file(file_id).is_valid = false;
      }
    }
    if ((files_[file_id.value]->kind == FileTypeKind::kClass ||
         files_[file_id.value]->kind == FileTypeKind::kStruct) &&
        function.name == "Main" && !function.is_static) {
      diagnostics_.error(function.range,
                         "entry point 'Main' must be declared static");
      model_.mutable_symbol(symbol).is_valid = false;
    }
    model_.mutable_file(file_id).functions.at(index) = symbol;
  }

  void validate_interface_contracts() {
    std::vector<bool> complete(files_.size(), false);
    std::size_t remaining = 0;
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (model_.file(FileId{index}).kind == FileTypeKind::kInterface) {
        ++remaining;
      } else {
        complete[index] = true;
      }
    }

    auto append_interface = [](std::vector<FileId>& interfaces,
                               FileId interface_file) {
      if (std::ranges::find(interfaces, interface_file) == interfaces.end()) {
        interfaces.push_back(interface_file);
      }
    };

    auto merge_function = [this](FileId interface_file,
                                 std::vector<SymbolId>& functions,
                                 SymbolId candidate_id) {
      const SemanticSymbol candidate = model_.symbol(candidate_id);
      if (!candidate.is_valid) {
        return;
      }
      const auto existing = std::ranges::find_if(
          functions, [this, &candidate](SymbolId symbol_id) {
            const SemanticSymbol& symbol = model_.symbol(symbol_id);
            return symbol.name == candidate.name &&
                   symbol.parameter_types == candidate.parameter_types;
          });
      if (existing == functions.end()) {
        functions.push_back(candidate_id);
        return;
      }

      const SemanticSymbol inherited = model_.symbol(*existing);
      SymbolId selected = *existing;
      if (is_override_return_compatible(inherited.type, candidate.type)) {
        selected = candidate_id;
      } else if (!is_override_return_compatible(candidate.type,
                                                inherited.type)) {
        diagnostics_.error(candidate.range,
                           "interface function '" +
                               callable_signature(candidate) +
                               "' conflicts with inherited return type '" +
                               type_name(inherited.type) + "'");
        diagnostics_.note(inherited.range,
                          "conflicting interface function is declared here");
        model_.mutable_file(interface_file).is_valid = false;
        return;
      }

      const std::vector<TypeId> permitted = intersect_error_sets(
          inherited.thrown_types.span(), candidate.thrown_types.span());
      if (model_.symbol(selected).thrown_types == permitted) {
        *existing = selected;
        return;
      }

      SemanticSymbol merged = model_.symbol(selected);
      merged.file = interface_file;
      merged.range = candidate.range;
      merged.thrown_types = permitted;
      merged.has_explicit_throws = true;
      merged.virtual_slot.reset();
      merged.overridden_symbol.reset();
      *existing = model_.add_symbol(std::move(merged));
    };

    while (remaining != 0) {
      bool made_progress = false;
      for (std::size_t index = 0; index < files_.size(); ++index) {
        const FileId file_id{index};
        FileSemantics& file = model_.mutable_file(file_id);
        if (complete[index] || file.kind != FileTypeKind::kInterface) {
          continue;
        }
        if (std::ranges::any_of(file.direct_interfaces, [&complete](
                                                            FileId parent) {
              return parent.value < complete.size() && !complete[parent.value];
            })) {
          continue;
        }

        std::vector<FileId> interfaces;
        std::vector<SymbolId> functions;
        for (const FileId parent : file.direct_interfaces) {
          const FileSemantics& parent_file = model_.file(parent);
          for (const FileId inherited : parent_file.interfaces) {
            append_interface(interfaces, inherited);
          }
          for (const SymbolId function : parent_file.interface_functions) {
            merge_function(file_id, functions, function);
          }
        }
        append_interface(interfaces, file_id);
        file.interfaces = interfaces;
        for (const SymbolId function : file.functions) {
          merge_function(file_id, functions, function);
        }
        file.interfaces = std::move(interfaces);
        file.interface_functions = std::move(functions);
        complete[index] = true;
        --remaining;
        made_progress = true;
      }
      if (made_progress) {
        continue;
      }

      for (std::size_t index = 0; index < files_.size(); ++index) {
        const FileId file_id{index};
        FileSemantics& file = model_.mutable_file(file_id);
        if (complete[index] || file.kind != FileTypeKind::kInterface) {
          continue;
        }
        diagnostics_.error(point_range(files_[index]->range.begin),
                           "interface inheritance cycle includes '" +
                               files_[index]->qualified_name + "'");
        file.is_valid = false;
        file.direct_interfaces.clear();
        file.interfaces = {file_id};
        file.interface_functions = file.functions;
        complete[index] = true;
        --remaining;
      }
    }
  }

  void validate_overrides() {
    std::vector<FileId> ordered_classes;
    std::vector<bool> complete(files_.size(), false);
    std::size_t remaining = files_.size();
    while (remaining != 0) {
      bool made_progress = false;
      for (std::size_t index = 0; index < files_.size(); ++index) {
        if (complete[index]) {
          continue;
        }
        const FileId file_id{index};
        if (model_.file(file_id).kind != FileTypeKind::kClass &&
            model_.file(file_id).kind != FileTypeKind::kError) {
          complete[index] = true;
          --remaining;
          made_progress = true;
          continue;
        }
        const std::optional<FileId> base = model_.file(file_id).base_file;
        if (base && base->value < complete.size() && !complete[base->value]) {
          continue;
        }
        FileSemantics& file = model_.mutable_file(file_id);
        std::vector<FileId> interfaces;
        if (base) interfaces = model_.file(*base).interfaces;
        for (const FileId direct : file.direct_interfaces) {
          for (const FileId inherited : model_.file(direct).interfaces) {
            if (std::ranges::find(interfaces, inherited) == interfaces.end()) {
              interfaces.push_back(inherited);
            }
          }
        }
        file.interfaces = std::move(interfaces);
        ordered_classes.push_back(file_id);
        complete[index] = true;
        --remaining;
        made_progress = true;
      }
      if (!made_progress) {
        break;
      }
    }
    // Return covariance may mention any class, including a later source file.
    // Complete every interface closure before validating functions base-first.
    for (const FileId file_id : ordered_classes) {
      validate_file_overrides(file_id);
    }
  }

  void validate_interface_conformance() {
    std::vector<bool> complete(files_.size(), false);
    std::size_t remaining = 0;
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (model_.file(FileId{index}).kind == FileTypeKind::kClass ||
          model_.file(FileId{index}).kind == FileTypeKind::kError) {
        ++remaining;
      } else {
        complete[index] = true;
      }
    }

    while (remaining != 0) {
      bool made_progress = false;
      for (std::size_t index = 0; index < files_.size(); ++index) {
        const FileId file_id{index};
        FileSemantics& file = model_.mutable_file(file_id);
        if (complete[index] || (file.kind != FileTypeKind::kClass &&
                                file.kind != FileTypeKind::kError)) {
          continue;
        }
        if (file.base_file && file.base_file->value < complete.size() &&
            !complete[file.base_file->value]) {
          continue;
        }

        std::vector<InterfaceImplementation> implementations;
        for (const FileId interface_file : file.interfaces) {
          const FileSemantics& contract = model_.file(interface_file);
          std::vector<SymbolId> functions;
          bool complete_contract = true;
          for (const SymbolId requirement_id : contract.interface_functions) {
            const SemanticSymbol& requirement = model_.symbol(requirement_id);
            const auto implementation = std::ranges::find_if(
                file.virtual_functions,
                [this, &requirement](SymbolId function_id) {
                  const SemanticSymbol& function = model_.symbol(function_id);
                  return function.name == requirement.name &&
                         function.parameter_types ==
                             requirement.parameter_types;
                });
            if (implementation == file.virtual_functions.end()) {
              complete_contract = false;
              if (!file.is_abstract) {
                diagnostics_.error(
                    point_range(files_[index]->range.begin),
                    "concrete file class '" + files_[index]->qualified_name +
                        "' does not implement interface function '" +
                        callable_signature(requirement) + "'");
                diagnostics_.note(
                    requirement.range,
                    "interface function contract is declared here");
                file.is_valid = false;
              }
              continue;
            }
            const SemanticSymbol& function = model_.symbol(*implementation);
            if (!is_override_return_compatible(requirement.type,
                                               function.type)) {
              complete_contract = false;
              diagnostics_.error(function.range,
                                 "implementation of interface function '" +
                                     requirement.name + "' returns '" +
                                     type_name(function.type) +
                                     "'; contract returns '" +
                                     type_name(requirement.type) + "'");
              diagnostics_.note(requirement.range,
                                "interface function contract is declared here");
              file.is_valid = false;
              continue;
            }
            if (!throws_set_covers(requirement.thrown_types.span(),
                                   function.thrown_types.span())) {
              complete_contract = false;
              diagnostics_.error(function.range,
                                 "implementation of interface function '" +
                                     requirement.name +
                                     "' widens its throws contract");
              diagnostics_.note(requirement.range,
                                "interface throws contract is declared here");
              file.is_valid = false;
              continue;
            }
            functions.push_back(*implementation);
          }
          if (complete_contract) {
            implementations.push_back(
                InterfaceImplementation{interface_file, std::move(functions)});
          }
        }
        file.interface_implementations = std::move(implementations);
        complete[index] = true;
        --remaining;
        made_progress = true;
      }
      if (!made_progress) {
        break;
      }
    }
  }

  void validate_file_overrides(FileId file_id) {
    FileSemantics& file = model_.mutable_file(file_id);
    std::vector<SymbolId> virtual_functions;
    if (file.base_file) {
      virtual_functions = model_.file(*file.base_file).virtual_functions;
    }

    for (const SymbolId symbol_id : file.functions) {
      SemanticSymbol& symbol = model_.mutable_symbol(symbol_id);
      if (symbol.is_static || symbol.visibility == Visibility::kPrivate) {
        if (symbol.is_override) {
          diagnostics_.error(symbol.range,
                             symbol.is_static
                                 ? "static function '" + symbol.name +
                                       "' cannot be declared override"
                                 : "private function '" + symbol.name +
                                       "' cannot be declared override");
          symbol.is_valid = false;
          file.is_valid = false;
        }
        continue;
      }

      std::optional<std::size_t> matching_slot;
      for (std::size_t slot = 0; slot < virtual_functions.size(); ++slot) {
        const SemanticSymbol& candidate =
            model_.symbol(virtual_functions[slot]);
        if (candidate.name == symbol.name &&
            candidate.parameter_types == symbol.parameter_types) {
          matching_slot = slot;
          break;
        }
      }

      if (!matching_slot) {
        std::optional<SymbolId> interface_requirement;
        for (const FileId interface_file : file.interfaces) {
          for (const SymbolId requirement_id :
               model_.file(interface_file).interface_functions) {
            const auto& requirement = model_.symbol(requirement_id);
            if (requirement.name == symbol.name &&
                requirement.parameter_types == symbol.parameter_types) {
              interface_requirement = requirement_id;
              break;
            }
          }
          if (interface_requirement) break;
        }
        if (symbol.is_override && !interface_requirement) {
          diagnostics_.error(symbol.range,
                             "function '" + symbol.name +
                                 "' does not override an inherited "
                                 "class or interface function");
          symbol.is_valid = false;
          file.is_valid = false;
        }
        if (!symbol.is_override && interface_requirement) {
          diagnostics_.error(
              symbol.range,
              "function '" + callable_signature(symbol) +
                  "' implements an interface function; add 'override'");
          diagnostics_.note(model_.symbol(*interface_requirement).range,
                            "interface function contract is declared here");
          symbol.is_valid = false;
          file.is_valid = false;
        }
        symbol.virtual_slot = virtual_functions.size();
        virtual_functions.push_back(symbol_id);
        continue;
      }

      const SemanticSymbol& inherited =
          model_.symbol(virtual_functions[*matching_slot]);
      if (inherited.is_final) {
        diagnostics_.error(symbol.range,
                           "function '" + symbol.name +
                               "' cannot override inherited final function");
        diagnostics_.note(inherited.range, "final function is declared here");
        symbol.is_valid = false;
        file.is_valid = false;
        continue;
      }
      if (!is_override_return_compatible(inherited.type, symbol.type)) {
        diagnostics_.error(symbol.range, "override of '" + symbol.name +
                                             "' returns '" +
                                             type_name(symbol.type) +
                                             "'; inherited function returns '" +
                                             type_name(inherited.type) + "'");
        diagnostics_.note(inherited.range,
                          "inherited function is declared here");
        symbol.is_valid = false;
        file.is_valid = false;
        continue;
      }
      if (!throws_set_covers(inherited.thrown_types.span(),
                             symbol.thrown_types.span())) {
        diagnostics_.error(symbol.range,
                           "override of '" + symbol.name +
                               "' widens the inherited throws contract");
        diagnostics_.note(inherited.range,
                          "inherited throws contract is declared here");
        symbol.is_valid = false;
        file.is_valid = false;
        continue;
      }
      if (!symbol.is_override) {
        diagnostics_.error(symbol.range, "function '" + symbol.name +
                                             "' overrides an inherited "
                                             "function; add 'override'");
        diagnostics_.note(inherited.range,
                          "inherited function is declared here");
        symbol.is_valid = false;
        file.is_valid = false;
      }
      symbol.virtual_slot = *matching_slot;
      symbol.overridden_symbol = virtual_functions[*matching_slot];
      virtual_functions[*matching_slot] = symbol_id;
    }
    file.virtual_functions = std::move(virtual_functions);
    file.abstract_functions.clear();
    for (const SymbolId symbol_id : file.virtual_functions) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.is_abstract && symbol.is_valid) {
        file.abstract_functions.push_back(symbol_id);
      }
    }
    if (!file.is_abstract) {
      for (const SymbolId symbol_id : file.abstract_functions) {
        const SemanticSymbol& symbol = model_.symbol(symbol_id);
        diagnostics_.error(point_range(files_[file_id.value]->range.begin),
                           "concrete file class '" +
                               files_[file_id.value]->qualified_name +
                               "' does not implement abstract function '" +
                               callable_signature(symbol) + "'");
        diagnostics_.note(symbol.range,
                          "abstract function declaration is here");
        file.is_valid = false;
      }
    }
  }

  void register_constructor(FileId file_id, std::size_t index) {
    const ConstructorDecl& constructor =
        files_[file_id.value]->constructors.at(index);
    std::vector<TypeId> parameters;
    parameters.reserve(constructor.parameters.size());
    bool valid = constructor.is_valid;
    for (const ParameterDecl& parameter : constructor.parameters) {
      const TypeId type = resolve_type(parameter.type, file_id);
      parameters.push_back(type);
      valid = valid && type != model_.error_type();
    }
    const FileSemantics& file = model_.file(file_id);
    const SymbolId symbol = model_.add_symbol(
        SemanticSymbol{SymbolKind::kConstructor, std::string{constructor.name},
                       file.type, std::move(parameters), constructor.visibility,
                       file_id, constructor.range, valid});
    model_.mutable_symbol(symbol).has_explicit_throws =
        constructor.has_explicit_throws;
    const std::size_t throws_diagnostic_begin =
        diagnostics_.diagnostics().size();
    model_.mutable_symbol(symbol).thrown_types = resolve_throws_types(
        constructor.throws_types, file_id, constructor.visibility);
    if (diagnostics_.diagnostics().size() != throws_diagnostic_begin) {
      model_.mutable_symbol(symbol).is_valid = false;
    }
    model_.mutable_file(file_id).constructors.at(index) = symbol;
  }

  TypeId resolve_type(const TypeSyntax& syntax, FileId current_file,
                      bool allow_void = false) {
    if (const std::optional<TypeId> core = model_.find_type(syntax.name);
        core && model_.type(*core).kind != TypeKind::kFileClass &&
        model_.type(*core).kind != TypeKind::kInterface &&
        model_.type(*core).kind != TypeKind::kEnum &&
        model_.type(*core).kind != TypeKind::kStruct) {
      if (*core == model_.void_type()) {
        if (syntax.is_array) {
          diagnostics_.error(syntax.range,
                             "'void' cannot be an array element type");
          model_.mutable_file(current_file).is_valid = false;
          return model_.error_type();
        }
        if (syntax.is_nullable) {
          diagnostics_.error(syntax.range, "'void' cannot be nullable");
          model_.mutable_file(current_file).is_valid = false;
          return model_.error_type();
        }
        if (!allow_void) {
          diagnostics_.error(syntax.range,
                             "'void' is only valid as a function return type");
          model_.mutable_file(current_file).is_valid = false;
          return model_.error_type();
        }
        return *core;
      }
      return apply_type_syntax(syntax, current_file, *core);
    }
    const std::optional<FileId> file =
        find_visible_file(current_file, syntax.name);
    if (!file) {
      if (syntax.name == "String") {
        diagnostics_.error(syntax.range,
                           "unknown type 'String'; use the built-in type "
                           "'string'");
        model_.mutable_file(current_file).is_valid = false;
        return model_.error_type();
      }
      if (const std::optional<FileId> inaccessible =
              find_inaccessible_file(current_file, syntax.name)) {
        diagnostics_.error(
            syntax.range,
            "file class '" +
                model_.symbol(model_.file(*inaccessible).symbol).name +
                "' is private");
        model_.mutable_file(current_file).is_valid = false;
        return model_.error_type();
      }
      diagnostics_.error(syntax.range,
                         "unknown type '" + std::string{syntax.name} + "'");
      model_.mutable_file(current_file).is_valid = false;
      return model_.error_type();
    }
    const TypeId type = model_.file(*file).type;
    return apply_type_syntax(syntax, current_file, type);
  }

  TypeId apply_type_syntax(const TypeSyntax& syntax, FileId current_file,
                           TypeId base_type) {
    TypeId type = base_type;
    if (syntax.is_element_nullable) {
      if (!is_reference(type)) {
        diagnostics_.error(syntax.range,
                           "nullable marker requires a reference type; '" +
                               type_name(type) + "' is a value type");
        model_.mutable_file(current_file).is_valid = false;
        return model_.error_type();
      }
      type = model_.get_nullable_type(type);
    }
    if (syntax.is_array) {
      type = model_.get_array_type(type);
    }
    if (syntax.is_nullable) {
      if (!is_reference(type)) {
        diagnostics_.error(syntax.range,
                           "nullable marker requires a reference type; '" +
                               type_name(type) + "' is a value type");
        model_.mutable_file(current_file).is_valid = false;
        return model_.error_type();
      }
      type = model_.get_nullable_type(type);
    }
    return type;
  }

  void analyze_definitions() {
    // Bind every static initializer before evaluating the dependency graph.
    // Canonical ordering also makes independent constant errors reproducible.
    std::vector<std::pair<std::string, std::pair<FileId, std::size_t>>>
        constants;
    for (std::size_t file = 0; file < files_.size(); ++file) {
      for (std::size_t field = 0; field < files_[file]->fields.size();
           ++field) {
        if (!files_[file]->fields[field].is_static) continue;
        const auto symbol = model_.file(FileId{file}).fields[field];
        constants.push_back(
            {canonical_symbol_identity(model_.symbol(symbol), model_,
                                       CanonicalMemberKind::kStaticField),
             {FileId{file}, field}});
      }
    }
    std::ranges::sort(
        constants, {},
        [](const auto& entry) -> const std::string& { return entry.first; });
    for (const auto& [identity, location] : constants) {
      static_cast<void>(identity);
      current_file_ = location.first;
      analyze_field(location.second);
    }
    evaluate_static_constants(files_, model_, diagnostics_);
    instance_field_initializers_complete_.assign(files_.size(), true);
    for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
      current_file_ = FileId{file_index};
      for (std::size_t index = 0; index < files_[file_index]->fields.size();
           ++index) {
        if (files_[file_index]->fields[index].is_static) continue;
        const std::size_t before = diagnostics_.diagnostics().size();
        analyze_field(index);
        if (diagnostics_.diagnostics().size() != before)
          model_.mutable_file(current_file_).is_valid = false;
      }
    }
    for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
      current_file_ = FileId{file_index};
      const std::size_t diagnostic_begin = diagnostics_.diagnostics().size();
      const FileClassDecl& syntax = *files_[file_index];
      for (const MemberReference& reference : syntax.member_order) {
        switch (reference.kind) {
          case DeclarationKind::kField:
            break;
          case DeclarationKind::kFunction:
            analyze_function(reference.index);
            break;
          case DeclarationKind::kConstructor:
            analyze_constructor(reference.index);
            break;
          case DeclarationKind::kNestedType:
            break;
        }
      }
      validate_field_initialization(syntax, model_, current_file_,
                                    diagnostics_);
      const auto diagnostics = diagnostics_.diagnostics();
      for (std::size_t index = diagnostic_begin; index < diagnostics.size();
           ++index) {
        if (diagnostics[index].severity == DiagnosticSeverity::kError) {
          model_.mutable_file(current_file_).is_valid = false;
          break;
        }
      }
    }
  }

  void analyze_field(std::size_t index) {
    const std::size_t diagnostic_begin = diagnostics_.diagnostics().size();
    const FieldDecl& field = files_[current_file_.value]->fields.at(index);
    const SymbolId symbol = model_.file(current_file_).fields.at(index);
    if (field.is_static) {
      auto& budget =
          constant_budgets_[model_.file(current_file_).identity.package.name];
      if (++budget.first > kMaxStaticConstants) {
        if (budget.first == kMaxStaticConstants + 1)
          diagnostics_.error(field.range,
                             "package exceeds 65536 static constants");
        model_.mutable_symbol(symbol).is_valid = false;
        model_.mutable_file(current_file_).is_valid = false;
        return;
      }
      if (!field.is_valid) {
        model_.mutable_symbol(symbol).is_valid = false;
        return;
      }
      if (field.initializer) {
        const auto count =
            preflight_constant_expression(files_[current_file_.value]->storage,
                                          *field.initializer, diagnostics_);
        if (!count || *count > kMaxPackageConstantNodes - budget.second) {
          if (count)
            diagnostics_.error(
                field.range,
                "package exceeds 1048576 constant expression nodes");
          model_.mutable_symbol(symbol).is_valid = false;
          model_.mutable_file(current_file_).is_valid = false;
          return;
        }
        budget.second += *count;
      }
      if (!field.is_final) {
        diagnostics_.error(field.range, "static field '" +
                                            std::string{field.name} +
                                            "' must also be final");
      }
      if (!field.initializer) {
        diagnostics_.error(field.range, "static field '" +
                                            std::string{field.name} +
                                            "' requires an initializer");
      }
    }
    if (!field.initializer) {
      if (field.is_static) model_.mutable_symbol(symbol).is_valid = false;
      return;
    }
    constant_context_ = field.is_static;
    const bool initializer_reachable =
        field.is_static ||
        instance_field_initializers_complete_.at(current_file_.value);
    if (!field.is_static) {
      current_effect_field_ = current_file_;
    }
    begin_root_scope(!field.is_static);
    const TypeId field_type = model_.symbol(symbol).type;
    const ExpressionState value = analyze_expression_with_effect_reachability(
        *field.initializer, initializer_reachable, field_type);
    if (!field.is_static && initializer_reachable &&
        value.type == model_.bottom_type()) {
      instance_field_initializers_complete_.at(current_file_.value) = false;
    }
    if (field.is_static && !is_static_scalar_type(field_type)) {
      diagnostics_.error(
          expression_range(*field.initializer),
          "static field initializer must be a scalar constant expression");
    }
    check_value(value, files_[current_file_.value]
                           ->storage.expression(*field.initializer)
                           .range);
    check_assignment(
        model_.symbol(model_.file(current_file_).fields.at(index)).type,
        value.type,
        files_[current_file_.value]
            ->storage.expression(*field.initializer)
            .range,
        "field initializer");
    if (field.is_static &&
        diagnostics_.diagnostics().size() != diagnostic_begin)
      model_.mutable_symbol(symbol).is_valid = false;
    constant_context_ = false;
    current_effect_field_.reset();
    end_root_scope();
  }

  void analyze_function(std::size_t index) {
    const FunctionDecl& function =
        files_[current_file_.value]->functions.at(index);
    const SymbolId symbol = model_.file(current_file_).functions.at(index);
    if (function.is_abstract) {
      begin_root_scope(true);
      register_parameters(symbol, function.parameters);
      end_root_scope();
      return;
    }
    analyze_callable(symbol, function.parameters, function.body,
                     model_.symbol(symbol).type, nullptr);
  }

  void analyze_constructor(std::size_t index) {
    const ConstructorDecl& constructor =
        files_[current_file_.value]->constructors.at(index);
    const SymbolId symbol = model_.file(current_file_).constructors.at(index);
    analyze_callable(
        symbol, constructor.parameters, constructor.body, model_.void_type(),
        constructor.initializer ? &*constructor.initializer : nullptr);
  }

  void analyze_callable(SymbolId callable_symbol,
                        std::span<const ParameterDecl> parameters, BlockId body,
                        TypeId return_type,
                        const ConstructorInitializer* initializer) {
    const bool is_constructor =
        model_.symbol(callable_symbol).kind == SymbolKind::kConstructor;
    const bool is_static = model_.symbol(callable_symbol).is_static;
    begin_root_scope(is_constructor || !is_static);
    register_parameters(callable_symbol, parameters);
    current_callable_symbol_ = callable_symbol;

    bool prefix_completes = true;
    if (is_constructor) {
      const bool base_completes =
          analyze_constructor_initializer(callable_symbol, initializer);
      prefix_completes =
          base_completes &&
          instance_field_initializers_complete_.at(current_file_.value);
      if (!base_completes) {
        incomplete_base_initializers_.push_back(callable_symbol);
      }
    }

    expected_return_type_ = return_type;
    current_callable_kind_ = model_.symbol(callable_symbol).kind;
    static_cast<void>(
        analyze_block_with_effect_reachability(body, false, prefix_completes));
    current_callable_kind_.reset();
    current_callable_symbol_.reset();
    expected_return_type_ = model_.void_type();
    end_root_scope();
  }

  void register_parameters(SymbolId callable_symbol,
                           std::span<const ParameterDecl> parameters) {
    const std::vector<TypeId> parameter_types =
        model_.symbol(callable_symbol).parameter_types;
    for (std::size_t index = 0; index < parameters.size(); ++index) {
      const ParameterDecl& parameter = parameters[index];
      const TypeId parameter_type = index < parameter_types.size()
                                        ? parameter_types[index]
                                        : model_.error_type();
      const SymbolId symbol = model_.add_symbol(
          SemanticSymbol{SymbolKind::kParameter,
                         std::string{parameter.name},
                         parameter_type,
                         {},
                         Visibility::kPrivate,
                         current_file_,
                         parameter.range,
                         parameter_type != model_.error_type()});
      model_.mutable_symbol(symbol).is_final = parameter.is_final;
      model_.mutable_symbol(callable_symbol)
          .parameter_symbols.push_back(symbol);
      bind_name(parameter.name, symbol, parameter.range);
    }
  }

  bool analyze_constructor_initializer(
      SymbolId constructor_symbol, const ConstructorInitializer* initializer) {
    const FileSemantics& file = model_.file(current_file_);
    if (!file.base_file) {
      if (file.kind == FileTypeKind::kError) {
        if (initializer == nullptr) {
          model_.mutable_symbol(constructor_symbol).base_constructor =
              error_default_constructor_;
          record_call_effect(error_default_constructor_,
                             model_.symbol(constructor_symbol).range);
          return true;
        }
        return analyze_error_root_initializer(constructor_symbol, *initializer);
      }
      if (initializer != nullptr) {
        diagnostics_.error(initializer->range,
                           "root class constructor cannot have a base "
                           "initializer");
      }
      return true;
    }

    const FileSemantics& base = model_.file(*file.base_file);
    const std::string& base_name = model_.symbol(base.symbol).name;
    if (initializer == nullptr) {
      if (model_.symbol(constructor_symbol).is_valid) {
        diagnostics_.error(model_.symbol(constructor_symbol).range,
                           "constructor for derived class '" +
                               model_.symbol(file.symbol).name +
                               "' must initialize base '" + base_name + "'");
      }
      return true;
    }

    const TypeId initialized_type =
        resolve_type(initializer->base_type, current_file_);
    if (initialized_type != base.type) {
      diagnostics_.error(
          initializer->base_type.range,
          "constructor initializer must name direct base '" + base_name + "'");
    }

    std::vector<ExpressionState> arguments;
    arguments.reserve(initializer->arguments.size());
    bool has_error_argument = false;
    bool has_bottom_argument = false;
    analyzing_base_initializer_ = true;
    for (const ExpressionId argument : initializer->arguments) {
      const bool previous = effects_reachable_;
      effects_reachable_ = previous && !has_bottom_argument;
      ExpressionState value = analyze_overload_argument(argument);
      effects_reachable_ = previous;
      check_value(value, expression_range(argument));
      has_error_argument =
          has_error_argument || value.type == model_.error_type();
      has_bottom_argument =
          has_bottom_argument || value.type == model_.bottom_type();
      arguments.push_back(std::move(value));
    }
    analyzing_base_initializer_ = false;

    if (initialized_type != base.type) {
      return !has_bottom_argument;
    }
    std::vector<SymbolId> matches;
    std::vector<SymbolId> exact_matches;
    for (const SymbolId candidate : base.constructors) {
      const SemanticSymbol& symbol = model_.symbol(candidate);
      if (!symbol.is_valid ||
          symbol.parameter_types.size() != arguments.size()) {
        continue;
      }
      bool matches_types = true;
      bool matches_exactly = true;
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        bool argument_is_exact = false;
        if (!overload_argument_matches(
                initializer->arguments[index], arguments[index],
                symbol.parameter_types[index], argument_is_exact)) {
          matches_types = false;
          break;
        }
        matches_exactly = matches_exactly && argument_is_exact;
      }
      if (matches_types) {
        matches.push_back(candidate);
        if (matches_exactly) {
          exact_matches.push_back(candidate);
        }
      }
    }
    if (!exact_matches.empty()) {
      matches = std::move(exact_matches);
    }
    if (has_error_argument) {
      return !has_bottom_argument;
    }
    if (matches.empty()) {
      diagnostics_.error(initializer->range,
                         "no matching base constructor '" + base_name +
                             "' for " + std::to_string(arguments.size()) +
                             " argument(s)");
      return !has_bottom_argument;
    }
    if (matches.size() > 1) {
      diagnostics_.error(initializer->range,
                         "base constructor call is ambiguous between " +
                             std::to_string(matches.size()) + " overloads");
      return !has_bottom_argument;
    }
    const SymbolId selected = matches.front();
    const SemanticSymbol& selected_symbol = model_.symbol(selected);
    if (selected_symbol.visibility == Visibility::kPrivate &&
        selected_symbol.file != current_file_) {
      diagnostics_.error(initializer->range,
                         "base constructor for '" + base_name + "' is private");
      diagnostics_.note(selected_symbol.range,
                        "private constructor is declared here");
      return !has_bottom_argument;
    }
    apply_overload_argument_context(initializer->arguments, arguments,
                                    selected_symbol.parameter_types);
    model_.mutable_symbol(constructor_symbol).base_constructor = selected;
    if (!has_bottom_argument) {
      record_call_effect(selected, initializer->range);
    }
    return !has_bottom_argument;
  }

  bool analyze_error_root_initializer(
      SymbolId constructor_symbol, const ConstructorInitializer& initializer) {
    const TypeId initialized_type =
        resolve_type(initializer.base_type, current_file_);
    analyzing_base_initializer_ = true;
    std::vector<ExpressionState> arguments;
    arguments.reserve(initializer.arguments.size());
    bool has_bottom_argument = false;
    for (const ExpressionId argument : initializer.arguments) {
      const bool previous = effects_reachable_;
      effects_reachable_ = previous && !has_bottom_argument;
      ExpressionState value = analyze_overload_argument(argument);
      effects_reachable_ = previous;
      check_value(value, expression_range(argument));
      has_bottom_argument =
          has_bottom_argument || value.type == model_.bottom_type();
      arguments.push_back(std::move(value));
    }
    analyzing_base_initializer_ = false;
    if (initialized_type != model_.error_root_type()) {
      diagnostics_.error(initializer.base_type.range,
                         "root error constructor initializer must name "
                         "compiler error 'Error'");
      return !has_bottom_argument;
    }
    SymbolId selected = error_default_constructor_;
    if (arguments.size() == 1 &&
        is_assignable(model_.string_type(), arguments.front().type)) {
      selected = error_message_constructor_;
    } else if (!arguments.empty()) {
      diagnostics_.error(initializer.range,
                         "no matching Error constructor for " +
                             std::to_string(arguments.size()) + " argument(s)");
      return !has_bottom_argument;
    }
    apply_overload_argument_context(initializer.arguments, arguments,
                                    model_.symbol(selected).parameter_types);
    model_.mutable_symbol(constructor_symbol).base_constructor = selected;
    if (!has_bottom_argument) {
      record_call_effect(selected, initializer.range);
    }
    return !has_bottom_argument;
  }

  void begin_root_scope(bool include_self = true) {
    scopes_.clear();
    active_non_null_.clear();
    scopes_.push_back(Scope{});
    current_scope_ = 0;
    has_implicit_receiver_ = include_self;
    if (include_self) {
      scopes_[0].entries.push_back(
          ScopeEntry{"self", model_.file(current_file_).self_symbol});
    }
  }

  void end_root_scope() {
    scopes_.clear();
    active_non_null_.clear();
    current_scope_.reset();
    has_implicit_receiver_ = false;
  }

  void push_scope() {
    const std::size_t id = scopes_.size();
    scopes_.push_back(Scope{current_scope_, {}});
    current_scope_ = id;
  }

  void pop_scope() {
    const Scope& scope = scopes_.at(*current_scope_);
    for (const ScopeEntry& entry : scope.entries) {
      std::erase(active_non_null_, entry.symbol);
    }
    current_scope_ = scope.parent;
  }

  void bind_name(std::string_view name, SymbolId symbol, SourceRange range) {
    Scope& scope = scopes_.at(*current_scope_);
    for (const ScopeEntry& entry : scope.entries) {
      if (entry.name != name) {
        continue;
      }
      diagnostics_.error(range,
                         "duplicate local name '" + std::string{name} + "'");
      diagnostics_.note(model_.symbol(entry.symbol).range,
                        "previous declaration is here");
      return;
    }
    scope.entries.push_back(ScopeEntry{name, symbol});
  }

  std::optional<SymbolId> lookup_local(std::string_view name) const {
    std::optional<std::size_t> scope = current_scope_;
    while (scope) {
      const Scope& current = scopes_.at(*scope);
      for (auto entry = current.entries.rbegin();
           entry != current.entries.rend(); ++entry) {
        if (entry->name == name) {
          return entry->symbol;
        }
      }
      scope = current.parent;
    }
    return std::nullopt;
  }

  static void merge_non_null(std::optional<NonNullSet>& join,
                             const NonNullSet& path) {
    join = join ? intersect_symbols(*join, path) : path;
  }

  void finish_loop(const NonNullSet& base, bool body_terminates) {
    auto paths = transfers_.back().breaks;
    if (transfers_.back().continues)
      merge_non_null(paths, *transfers_.back().continues);
    if (!body_terminates) merge_non_null(paths, active_non_null_);
    active_non_null_ = paths ? intersect_symbols(base, *paths) : base;
    transfers_.pop_back();
  }

  ExpressionId ungroup(ExpressionId id) const {
    const AstStorage& storage = files_[current_file_.value]->storage;
    while (const auto* group = std::get_if<ParenthesizedExpression>(
               &storage.expression(id).data)) {
      id = group->expression;
    }
    return id;
  }

  bool is_true_literal(ExpressionId id) const {
    const auto* literal = std::get_if<LiteralExpression>(
        &files_[current_file_.value]->storage.expression(ungroup(id)).data);
    return literal && literal->kind == LiteralKind::kBoolean &&
           literal->lexeme == "true";
  }

  std::optional<ScalarConstant> integer_case_literal(ExpressionId id,
                                                     TypeId type) const {
    const AstStorage& storage = files_[current_file_.value]->storage;
    id = ungroup(id);
    bool negative = false;
    if (const auto* unary =
            std::get_if<UnaryExpression>(&storage.expression(id).data)) {
      if (unary->operation != TokenKind::kMinus) return std::nullopt;
      negative = true;
      id = ungroup(unary->operand);
    }
    const auto* literal =
        std::get_if<LiteralExpression>(&storage.expression(id).data);
    if (!literal || literal->kind != LiteralKind::kInteger) return std::nullopt;
    const NumericLiteralSpelling spelling =
        parse_numeric_literal_spelling(literal->lexeme);
    if (spelling.error != NumericLiteralSpellingError::kNone) {
      return std::nullopt;
    }
    const auto bits =
        integer_constant_bits(spelling.core, negative, model_.type(type).kind);
    return bits ? std::optional{ScalarConstant{type, *bits}} : std::nullopt;
  }

  std::optional<SwitchLabel> analyze_case_label(ExpressionId id,
                                                TypeId selector_type) {
    const NonNullSet before = active_non_null_;
    const ExpressionState state =
        analyze_expression_with_effect_reachability(id, false, selector_type);
    active_non_null_ = before;  // Labels never execute, including invalid ones.
    const SourceRange range = expression_range(id);
    if (!check_value(state, range) || state.type == model_.error_type())
      return std::nullopt;
    if (const auto literal = integer_case_literal(id, state.type)) {
      if (literal->type == selector_type) {
        return SwitchLabel{*literal, range};
      }
      const auto widened =
          widen_integer_constant(literal->bits, model_.type(literal->type).kind,
                                 model_.type(selector_type).kind);
      if (widened) {
        return SwitchLabel{ScalarConstant{selector_type, *widened}, range};
      }
      diagnostics_.error(range, "case constant of type '" +
                                    type_name(literal->type) +
                                    "' cannot be used for switch selector '" +
                                    type_name(selector_type) + "'");
      return std::nullopt;
    }
    const Expression& syntax =
        files_[current_file_.value]->storage.expression(ungroup(id));
    if (state.symbol &&
        (std::holds_alternative<IdentifierExpression>(syntax.data) ||
         std::holds_alternative<MemberAccessExpression>(syntax.data))) {
      const SemanticSymbol& symbol = model_.symbol(*state.symbol);
      std::optional<ScalarConstant> value;
      if (symbol.kind == SymbolKind::kEnumCase && symbol.enum_tag &&
          std::holds_alternative<MemberAccessExpression>(syntax.data)) {
        value = ScalarConstant{symbol.type, *symbol.enum_tag};
      } else if (symbol.kind == SymbolKind::kField && symbol.is_static &&
                 symbol.is_final) {
        value = symbol.static_constant;
      }
      if (value) {
        if (value->type == selector_type)
          return SwitchLabel{*value, range, state.symbol};
        const auto widened =
            widen_integer_constant(value->bits, model_.type(value->type).kind,
                                   model_.type(selector_type).kind);
        if (widened)
          return SwitchLabel{ScalarConstant{selector_type, *widened}, range,
                             state.symbol};
        diagnostics_.error(range, "case constant of type '" +
                                      type_name(value->type) +
                                      "' cannot be used for switch selector '" +
                                      type_name(selector_type) + "'");
        return std::nullopt;
      }
    }
    diagnostics_.error(
        range,
        "case label must be an integer literal, a qualified enum case, or a "
        "verified static final integer/enum constant");
    return std::nullopt;
  }

  bool analyze_switch(StatementId id, const SwitchStatement& selection,
                      SourceRange range) {
    const ExpressionState selector = analyze_expression(selection.selector);
    check_value(selector, expression_range(selection.selector));
    const TypeKind kind = model_.type(selector.type).kind;
    const bool valid_selector =
        is_integer_type(kind) || kind == TypeKind::kEnum;
    const bool selector_completes = selector.type != model_.bottom_type();
    if (!valid_selector && selector.type != model_.error_type() &&
        selector_completes) {
      diagnostics_.error(expression_range(selection.selector),
                         "switch selector must be an enum or integer value");
    }
    SwitchSemantics checked{selector.type, {}, false};
    std::map<std::uint64_t, SourceRange> values;
    bool has_default = false;
    std::size_t label_count = 0;
    for (const SwitchArm& arm : selection.arms) {
      std::vector<SwitchLabel> labels;
      has_default = has_default || arm.labels.empty();
      for (const ExpressionId label : arm.labels) {
        ++label_count;
        if (!selector_completes) {
          static_cast<void>(
              analyze_expression_with_effect_reachability(label, false));
          continue;
        }
        if (auto value = analyze_case_label(label, selector.type);
            value && valid_selector) {
          const auto [first, inserted] =
              values.emplace(value->value.bits, value->range);
          if (!inserted) {
            diagnostics_.error(value->range, "duplicate switch case value");
            diagnostics_.note(first->second,
                              "first case with this value is here");
          }
          labels.push_back(*value);
        }
      }
      checked.labels.push_back(std::move(labels));
    }
    if (label_count > kMaxSwitchLabels ||
        selection.arms.size() > kMaxSwitchArms)
      diagnostics_.error(range, "switch exceeds case limits");
    checked.is_exhaustive = has_default;
    if (kind == TypeKind::kEnum) {
      const auto& cases =
          model_.file(*model_.type(selector.type).file).enum_cases;
      std::size_t missing = 0;
      std::string names;
      for (const SymbolId case_id : cases) {
        const SemanticSymbol& symbol = model_.symbol(case_id);
        if (symbol.enum_tag && !values.contains(*symbol.enum_tag)) {
          if (missing < 8) {
            if (!names.empty()) names += ", ";
            names += symbol.name;
          }
          ++missing;
        }
      }
      checked.is_exhaustive = has_default || missing == 0;
      if (!checked.is_exhaustive) {
        if (missing > 8)
          names += " (and " + std::to_string(missing - 8) + " more)";
        diagnostics_.error(
            range, "non-exhaustive enum switch; missing cases: " + names);
      }
    }
    const NonNullSet entry = active_non_null_;
    std::optional<NonNullSet> join;
    if (!checked.is_exhaustive) join = entry;
    transfers_.push_back(TransferContext{false});
    for (const SwitchArm& arm : selection.arms) {
      active_non_null_ = entry;
      if (!analyze_block_with_effect_reachability(arm.body, true,
                                                  selector_completes))
        merge_non_null(join, active_non_null_);
    }
    if (transfers_.back().breaks)
      merge_non_null(join, *transfers_.back().breaks);
    transfers_.pop_back();
    active_non_null_ = join.value_or(entry);
    model_.mutable_file(current_file_)
        .switches.emplace(id.value, std::move(checked));
    return !selector_completes || !join;
  }

  bool analyze_block(BlockId id, bool create_scope) {
    if (create_scope) {
      push_scope();
    }
    bool definitely_returns = false;
    const bool entry_reachable = flow_reachable_;
    const Block& block = files_[current_file_.value]->storage.block(id);
    for (const StatementId statement : block.statements) {
      const NonNullSet before = active_non_null_;
      flow_reachable_ = entry_reachable && !definitely_returns;
      const bool statement_returns = analyze_statement(statement);
      if (definitely_returns) active_non_null_ = before;
      definitely_returns = definitely_returns || statement_returns;
    }
    flow_reachable_ = entry_reachable;
    if (create_scope) {
      pop_scope();
    }
    return definitely_returns;
  }

  bool analyze_block_with_effect_reachability(BlockId id, bool create_scope,
                                              bool reachable) {
    const bool previous = effects_reachable_;
    effects_reachable_ = previous && reachable;
    const bool result = analyze_block(id, create_scope);
    effects_reachable_ = previous;
    return result;
  }

  bool analyze_statement(StatementId id) {
    const Statement& statement =
        files_[current_file_.value]->storage.statement(id);
    if (std::holds_alternative<InvalidStatement>(statement.data)) {
      return false;
    }
    if (const auto* local =
            std::get_if<LocalVariableStatement>(&statement.data)) {
      TypeId type = local->type ? resolve_type(*local->type, current_file_)
                                : model_.error_type();
      std::optional<ExpressionState> initializer;
      if (local->initializer) {
        initializer = analyze_expression(
            *local->initializer,
            local->type ? std::optional<TypeId>{type} : std::nullopt);
        check_value(*initializer, expression_range(*local->initializer));
      }
      if (local->type) {
        if (initializer) {
          check_assignment(type, initializer->type,
                           expression_range(*local->initializer),
                           "local initializer");
        }
      } else if (!initializer) {
        if (!local->is_final) {
          diagnostics_.error(statement.range, "inferred local '" +
                                                  std::string{local->name} +
                                                  "' requires an initializer");
        }
      } else if (initializer->type == model_.null_type()) {
        diagnostics_.error(expression_range(*local->initializer),
                           "cannot infer the type of local '" +
                               std::string{local->name} + "' from null");
      } else if (initializer->type == model_.bottom_type()) {
        diagnostics_.error(expression_range(*local->initializer),
                           "cannot infer the type of local '" +
                               std::string{local->name} +
                               "' from a throw expression");
      } else if (initializer->type != model_.void_type()) {
        type = initializer->type;
      }
      if (!initializer && !local->is_final &&
          (model_.type(type).kind == TypeKind::kEnum ||
           model_.type(type).kind == TypeKind::kStruct)) {
        diagnostics_.error(statement.range,
                           std::string{type_kind_name(model_.type(type).kind)} +
                               " local '" + std::string{local->name} +
                               "' requires an initializer");
      }
      if (local->is_final && !initializer) {
        diagnostics_.error(statement.range, "final local '" +
                                                std::string{local->name} +
                                                "' requires an initializer");
      }
      const SymbolId symbol =
          model_.add_symbol(SemanticSymbol{SymbolKind::kLocal,
                                           std::string{local->name},
                                           type,
                                           {},
                                           Visibility::kPrivate,
                                           current_file_,
                                           statement.range,
                                           type != model_.error_type()});
      model_.mutable_symbol(symbol).is_final = local->is_final;
      model_.mutable_file(current_file_).statement_symbols.at(id.value) =
          symbol;
      bind_name(local->name, symbol, statement.range);
      return initializer && initializer->type == model_.bottom_type();
    }
    if (const auto* return_statement =
            std::get_if<ReturnStatement>(&statement.data)) {
      if (!return_statement->value) {
        if (expected_return_type_ != model_.void_type() &&
            expected_return_type_ != model_.error_type()) {
          diagnostics_.error(statement.range,
                             "return statement requires a value of type '" +
                                 type_name(expected_return_type_) + "'");
        }
      } else {
        const ExpressionState value = analyze_expression(
            *return_statement->value,
            expected_return_type_ == model_.void_type()
                ? std::nullopt
                : std::optional<TypeId>{expected_return_type_});
        check_value(value, expression_range(*return_statement->value));
        if (expected_return_type_ == model_.void_type() &&
            value.type != model_.bottom_type()) {
          diagnostics_.error(statement.range,
                             "cannot return a value from a void function");
        } else {
          check_assignment(expected_return_type_, value.type,
                           expression_range(*return_statement->value),
                           "return value");
        }
      }
      return true;
    }
    if (const auto* expression_statement =
            std::get_if<ExpressionStatement>(&statement.data)) {
      const ExpressionState value =
          analyze_expression(expression_statement->expression);
      if (value.category == ValueCategory::kCallable) {
        diagnostics_.error(statement.range,
                           "function reference must be called");
      } else if (value.category == ValueCategory::kType) {
        diagnostics_.error(statement.range,
                           "type reference cannot be used as a statement");
      } else if (value.category == ValueCategory::kSuper) {
        diagnostics_.error(statement.range,
                           "'super' must qualify an instance function call");
      }
      return value.type == model_.bottom_type();
    }
    if (const auto* if_statement = std::get_if<IfStatement>(&statement.data)) {
      const ExpressionState condition =
          analyze_expression(if_statement->condition);
      check_condition(condition, if_statement->condition, "if condition");
      const bool condition_completes = condition.type != model_.bottom_type();

      const ConditionFacts facts = condition_facts(if_statement->condition);
      const NonNullSet branch_base = active_non_null_;
      add_non_null_facts(facts.when_true);
      const bool then_returns = analyze_block_with_effect_reachability(
          if_statement->then_block, true, condition_completes);
      const NonNullSet then_state = active_non_null_;

      active_non_null_ = branch_base;
      add_non_null_facts(facts.when_false);
      bool else_returns = false;
      if (if_statement->else_block) {
        else_returns = analyze_block_with_effect_reachability(
            *if_statement->else_block, true, condition_completes);
      }
      const NonNullSet else_state = active_non_null_;

      if (then_returns && !else_returns) {
        active_non_null_ = else_state;
      } else if (!then_returns && else_returns) {
        active_non_null_ = then_state;
      } else if (!then_returns && !else_returns) {
        active_non_null_ = intersect_symbols(then_state, else_state);
      } else {
        active_non_null_ = branch_base;
      }
      return !condition_completes ||
             (then_returns && if_statement->else_block && else_returns);
    }
    if (const auto* while_statement =
            std::get_if<WhileStatement>(&statement.data)) {
      const ExpressionState condition =
          analyze_expression(while_statement->condition);
      check_condition(condition, while_statement->condition, "while condition");
      const bool condition_completes = condition.type != model_.bottom_type();
      const ConditionFacts facts = condition_facts(while_statement->condition);
      const NonNullSet loop_base = active_non_null_;
      add_non_null_facts(facts.when_true);
      transfers_.push_back(TransferContext{true});
      const bool body_returns = analyze_block_with_effect_reachability(
          while_statement->body, true, condition_completes);
      const bool terminates = is_true_literal(while_statement->condition) &&
                              !transfers_.back().breaks;
      finish_loop(loop_base, body_returns);
      return !condition_completes || terminates;
    }
    if (const auto* for_statement =
            std::get_if<ForEachStatement>(&statement.data)) {
      const ExpressionState iterable =
          analyze_expression(for_statement->iterable);
      check_value(iterable, expression_range(for_statement->iterable));

      TypeId element_type = model_.error_type();
      const bool iterable_completes = iterable.type != model_.bottom_type();
      if (iterable.type != model_.error_type() && iterable_completes) {
        const SemanticType& type = model_.type(iterable.type);
        if (type.kind == TypeKind::kArray && type.element_type) {
          element_type = *type.element_type;
        } else {
          diagnostics_.error(expression_range(for_statement->iterable),
                             "type '" + type.name + "' is not iterable");
        }
      }

      TypeId variable_type = element_type;
      if (for_statement->variable.type) {
        variable_type =
            resolve_type(*for_statement->variable.type, current_file_);
        check_assignment(variable_type, element_type,
                         for_statement->variable.range,
                         "for iteration variable");
      }
      const SymbolId symbol = model_.add_symbol(
          SemanticSymbol{SymbolKind::kLocal,
                         std::string{for_statement->variable.name},
                         variable_type,
                         {},
                         Visibility::kPrivate,
                         current_file_,
                         for_statement->variable.range,
                         variable_type != model_.error_type()});
      model_.mutable_symbol(symbol).is_final = for_statement->variable.is_final;
      model_.mutable_file(current_file_).statement_symbols.at(id.value) =
          symbol;

      push_scope();
      bind_name(for_statement->variable.name, symbol,
                for_statement->variable.range);
      const NonNullSet loop_base = active_non_null_;
      transfers_.push_back(TransferContext{true});
      const bool body_returns = analyze_block_with_effect_reachability(
          for_statement->body, false, iterable_completes);
      finish_loop(loop_base, body_returns);
      pop_scope();
      return !iterable_completes;
    }
    if (const auto* for_statement =
            std::get_if<ForStatement>(&statement.data)) {
      push_scope();
      bool prefix_completes = true;
      if (for_statement->initializer) {
        prefix_completes = !analyze_statement(*for_statement->initializer);
      }

      ConditionFacts facts;
      bool condition_completes = true;
      if (for_statement->condition) {
        const ExpressionState condition =
            analyze_expression_with_effect_reachability(
                *for_statement->condition, prefix_completes);
        check_condition(condition, *for_statement->condition, "for condition");
        condition_completes = condition.type != model_.bottom_type();
        facts = condition_facts(*for_statement->condition);
      }
      const NonNullSet loop_base = active_non_null_;
      add_non_null_facts(facts.when_true);

      transfers_.push_back(TransferContext{true});
      const bool body_returns = analyze_block_with_effect_reachability(
          for_statement->body, true, prefix_completes && condition_completes);
      auto update_entry = transfers_.back().continues;
      if (!body_returns) merge_non_null(update_entry, active_non_null_);
      active_non_null_ = update_entry.value_or(loop_base);
      bool updates_complete = true;
      for (const ExpressionId update : for_statement->updates) {
        const ExpressionState value =
            analyze_expression_with_effect_reachability(
                update, prefix_completes && condition_completes &&
                            updates_complete && update_entry.has_value());
        updates_complete =
            updates_complete && value.type != model_.bottom_type();
      }
      // continue paths run the updates; breaks do not.
      transfers_.back().continues.reset();
      const bool terminates = (!for_statement->condition ||
                               is_true_literal(*for_statement->condition)) &&
                              !transfers_.back().breaks;
      finish_loop(loop_base, !update_entry);
      pop_scope();
      return !prefix_completes || !condition_completes || terminates;
    }
    if (const auto* selection = std::get_if<SwitchStatement>(&statement.data)) {
      return analyze_switch(id, *selection, statement.range);
    }
    if (std::holds_alternative<BreakStatement>(statement.data)) {
      if (transfers_.empty()) {
        diagnostics_.error(statement.range,
                           "'break' is only valid inside a loop or switch");
      } else if (flow_reachable_) {
        merge_non_null(transfers_.back().breaks, active_non_null_);
      }
      return true;
    }
    if (std::holds_alternative<ContinueStatement>(statement.data)) {
      auto loop = std::find_if(
          transfers_.rbegin(), transfers_.rend(),
          [](const TransferContext& context) { return context.is_loop; });
      if (loop == transfers_.rend()) {
        diagnostics_.error(statement.range,
                           "'continue' is only valid inside a loop");
      } else if (flow_reachable_) {
        merge_non_null(loop->continues, active_non_null_);
      }
      return true;
    }
    const auto& nested = std::get<NestedBlockStatement>(statement.data);
    return analyze_block(nested.block, true);
  }

  ExpressionState analyze_expression(
      ExpressionId id, std::optional<TypeId> expected = std::nullopt,
      bool is_negated_literal = false) {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    return record_expression(
        id,
        analyze_expression_value(id, expression, expected, is_negated_literal));
  }

  ExpressionState analyze_expression_with_effect_reachability(
      ExpressionId id, bool reachable,
      std::optional<TypeId> expected = std::nullopt,
      bool is_negated_literal = false) {
    const bool previous = effects_reachable_;
    effects_reachable_ = previous && reachable;
    ExpressionState result =
        analyze_expression(id, expected, is_negated_literal);
    effects_reachable_ = previous;
    return result;
  }

  ExpressionState analyze_expression_value(ExpressionId id,
                                           const Expression& expression,
                                           std::optional<TypeId> expected,
                                           bool is_negated_literal) {
    // Instantiate one dispatch branch per node type so recursive checking does
    // not reserve stack space for every variant's result and temporaries.
    return std::visit(
        [&](const auto& node) -> ExpressionState {
          using Node = std::decay_t<decltype(node)>;
          if constexpr (std::is_same_v<Node, InvalidExpression>)
            return ExpressionState{model_.error_type()};
          else if constexpr (std::is_same_v<Node, IdentifierExpression>)
            return analyze_identifier(node, expression.range);
          else if constexpr (std::is_same_v<Node, SuperExpression>)
            return analyze_super(expression.range);
          else if constexpr (std::is_same_v<Node, ThrowExpression>)
            return analyze_throw(node, expression.range);
          else if constexpr (std::is_same_v<Node, LiteralExpression>)
            return analyze_literal(node, expression.range, expected,
                                   is_negated_literal);
          else if constexpr (std::is_same_v<Node, UnaryExpression>)
            return analyze_unary_chain(id, expected);
          else if constexpr (std::is_same_v<Node, UpdateExpression>)
            return analyze_update(node, expression.range);
          else if constexpr (std::is_same_v<Node, BinaryExpression>)
            return analyze_binary_chain(id, expected);
          else if constexpr (std::is_same_v<Node, TypeTestExpression>)
            return analyze_type_test(node, expression.range);
          else if constexpr (std::is_same_v<Node, CheckedCastExpression>)
            return analyze_checked_cast(node, expression.range);
          else if constexpr (std::is_same_v<Node, NumericConversionExpression>)
            return analyze_numeric_conversion(node, expression.range);
          else if constexpr (std::is_same_v<Node, IntegerConversionExpression>)
            return analyze_integer_conversion(node, expression.range);
          else if constexpr (std::is_same_v<Node, AssignmentExpression>)
            return analyze_assignment(node, expression.range);
          else if constexpr (std::is_same_v<Node, MemberAccessExpression>)
            return analyze_member_access(node, expression.range);
          else if constexpr (std::is_same_v<Node, MetaAccessExpression>)
            return analyze_meta_access(node, expression.range);
          else if constexpr (std::is_same_v<Node, SafeMemberAccessExpression>)
            return analyze_safe_member_access(node, expression.range);
          else if constexpr (std::is_same_v<Node, NullCoalesceExpression>)
            return analyze_null_coalesce(node, expression.range);
          else if constexpr (std::is_same_v<Node, NullAssertExpression>)
            return analyze_null_assert(node, expression.range);
          else if constexpr (std::is_same_v<Node, CallExpression>)
            return analyze_call(node, expression.range);
          else if constexpr (std::is_same_v<Node, ArrayLiteralExpression>)
            return analyze_array_literal(node, expression.range, expected);
          else if constexpr (std::is_same_v<Node, IndexExpression>)
            return analyze_index(node, expression.range);
          else if constexpr (std::is_same_v<Node, ParenthesizedExpression>)
            return analyze_expression(node.expression, expected,
                                      is_negated_literal);
        },
        expression.data);
  }

  ExpressionState analyze_throw(const ThrowExpression& thrown,
                                SourceRange range) {
    const ExpressionState operand = analyze_expression(thrown.operand);
    if (!check_value(operand, expression_range(thrown.operand))) {
      return ExpressionState{model_.error_type()};
    }
    if (operand.type == model_.bottom_type()) {
      diagnostics_.error(range, "throw operand never produces an error value");
      return ExpressionState{model_.error_type()};
    }
    if (!is_error_type(operand.type)) {
      diagnostics_.error(expression_range(thrown.operand),
                         "throw operand must be a non-null error; found '" +
                             type_name(operand.type) + "'");
      return ExpressionState{model_.error_type()};
    }
    if (constant_context_) {
      diagnostics_.error(range,
                         "throw expressions are not permitted in constant "
                         "initializers");
      return ExpressionState{model_.error_type()};
    }
    record_direct_error(operand.type, range);
    return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
  }

  ExpressionState analyze_identifier(const IdentifierExpression& identifier,
                                     SourceRange range) {
    if (const std::optional<SymbolId> local = lookup_local(identifier.name)) {
      const SemanticSymbol& symbol = model_.symbol(*local);
      if (analyzing_base_initializer_ && symbol.kind == SymbolKind::kSelf) {
        diagnostics_.error(
            range, "cannot use 'self' in a base constructor initializer");
        return ExpressionState{model_.error_type()};
      }
      ValueCategory category = ValueCategory::kMutableLocation;
      if (symbol.kind == SymbolKind::kSelf) {
        category = self_category();
      } else if (symbol.is_final &&
                 model_.type(symbol.type).kind == TypeKind::kStruct) {
        category = ValueCategory::kReadOnlyLocation;
      }
      return ExpressionState{
          narrowed_type(*local).value_or(symbol.type), category, *local, {}};
    }

    std::vector<SymbolId> members =
        find_members(current_file_, identifier.name);
    if (!members.empty()) {
      const SemanticSymbol& first = model_.symbol(members.front());
      if (first.kind == SymbolKind::kField) {
        if (analyzing_base_initializer_ && !first.is_static) {
          diagnostics_.error(range, "cannot use instance field '" + first.name +
                                        "' in a base constructor initializer");
          return ExpressionState{model_.error_type()};
        }
        if (!first.is_static && !has_implicit_receiver_) {
          diagnostics_.error(range, "instance field '" + first.name +
                                        "' is unavailable in a static context");
          return ExpressionState{model_.error_type()};
        }
        return ExpressionState{first.type,
                               field_category(first, self_category()),
                               members.front(),
                               {}};
      }
      return ExpressionState{model_.error_type(),
                             ValueCategory::kCallable,
                             {},
                             std::move(members)};
    }

    if (has_inaccessible_member(current_file_, identifier.name)) {
      diagnostics_.error(
          range,
          "inherited member '" + std::string{identifier.name} + "' is private");
      return ExpressionState{model_.error_type()};
    }

    if (const std::optional<FileId> file =
            find_visible_file(current_file_, identifier.name)) {
      const SemanticSymbol& symbol = model_.symbol(model_.file(*file).symbol);
      if (*file != current_file_ && symbol.visibility == Visibility::kPrivate) {
        diagnostics_.error(range,
                           "file class '" + symbol.name + "' is private");
        return ExpressionState{model_.error_type()};
      }
      return ExpressionState{
          symbol.type, ValueCategory::kType, model_.file(*file).symbol, {}};
    }

    if (const std::optional<TypeId> type = model_.find_type(identifier.name);
        type && is_builtin_type_expression(model_.type(*type).kind)) {
      return ExpressionState{*type, ValueCategory::kType};
    }

    if (identifier.name == "Error") {
      return ExpressionState{model_.error_root_type(), ValueCategory::kType};
    }
    if (identifier.name == "DivisionByZero") {
      return ExpressionState{model_.division_by_zero_type(),
                             ValueCategory::kType};
    }

    if (const std::optional<FileId> inaccessible =
            find_inaccessible_file(current_file_, identifier.name)) {
      diagnostics_.error(
          range, "file class '" +
                     model_.symbol(model_.file(*inaccessible).symbol).name +
                     "' is private");
      return ExpressionState{model_.error_type()};
    }

    std::vector<SymbolId> intrinsics = model_.find_intrinsics(identifier.name);
    std::erase_if(intrinsics, [this](SymbolId symbol) {
      const IntrinsicKind intrinsic = model_.symbol(symbol).intrinsic;
      return intrinsic == IntrinsicKind::kPrimitiveParse ||
             (intrinsic == IntrinsicKind::kConsoleReadLine &&
              !is_standard_library_console(current_file_));
    });
    if (!intrinsics.empty()) {
      return ExpressionState{model_.error_type(),
                             ValueCategory::kCallable,
                             {},
                             std::move(intrinsics)};
    }

    diagnostics_.error(range,
                       "unknown name '" + std::string{identifier.name} + "'");
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_super(SourceRange range) {
    const std::optional<FileId> direct_base =
        model_.file(current_file_).base_file;
    if (!direct_base) {
      diagnostics_.error(
          range, "'super' is unavailable because file class '" +
                     model_.symbol(model_.file(current_file_).symbol).name +
                     "' has no base");
      return ExpressionState{model_.error_type()};
    }
    if (analyzing_base_initializer_) {
      diagnostics_.error(
          range, "'super' cannot be used in a base constructor initializer");
      return ExpressionState{model_.error_type()};
    }
    if (!has_implicit_receiver_) {
      diagnostics_.error(range, "'super' is unavailable in a static context");
      return ExpressionState{model_.error_type()};
    }
    return ExpressionState{model_.file(*direct_base).type,
                           ValueCategory::kSuper};
  }

  ExpressionState analyze_literal(const LiteralExpression& literal,
                                  SourceRange range,
                                  std::optional<TypeId> expected,
                                  bool is_negated) {
    const NumericLiteralSpelling spelling =
        parse_numeric_literal_spelling(literal.lexeme);
    if ((literal.kind == LiteralKind::kInteger ||
         literal.kind == LiteralKind::kFloat) &&
        spelling.error != NumericLiteralSpellingError::kNone) {
      return ExpressionState{model_.error_type()};
    }
    switch (literal.kind) {
      case LiteralKind::kInteger: {
        TypeId type = *model_.find_type("int");
        if (spelling.suffix_kind != NumericLiteralSuffix::kNone) {
          type = *model_.find_type(
              numeric_literal_suffix_type_name(spelling.suffix_kind));
        } else if (expected && is_integer(*expected)) {
          type = *expected;
        }
        if ((!defer_default_numeric_literal_range_ || expected) &&
            !integer_literal_fits(spelling.core, type, is_negated)) {
          diagnostics_.error(
              range, "integer literal '" + std::string{is_negated ? "-" : ""} +
                         std::string{literal.lexeme} +
                         "' is out of range for '" + type_name(type) + "'");
          return ExpressionState{model_.error_type()};
        }
        return ExpressionState{type, ValueCategory::kValue};
      }
      case LiteralKind::kFloat: {
        TypeId type = *model_.find_type("float64");
        if (spelling.suffix_kind != NumericLiteralSuffix::kNone) {
          type = *model_.find_type(
              numeric_literal_suffix_type_name(spelling.suffix_kind));
        } else if (expected && is_floating_point(*expected)) {
          type = *expected;
        }
        if ((!defer_default_numeric_literal_range_ || expected) &&
            !floating_literal_fits(spelling.core, type)) {
          diagnostics_.error(
              range, "floating literal '" + std::string{literal.lexeme} +
                         "' is out of range for '" + type_name(type) + "'");
          return ExpressionState{model_.error_type()};
        }
        return ExpressionState{type, ValueCategory::kValue};
      }
      case LiteralKind::kString:
        if (!utf8_scalar_count(decode_string_literal(literal.lexeme))) {
          diagnostics_.error(range, "string literal is not valid UTF-8");
          return ExpressionState{model_.error_type()};
        }
        return ExpressionState{model_.string_type(), ValueCategory::kValue};
      case LiteralKind::kCharacter:
        return ExpressionState{*model_.find_type("char"),
                               ValueCategory::kValue};
      case LiteralKind::kBoolean:
        return ExpressionState{model_.bool_type(), ValueCategory::kValue};
      case LiteralKind::kNull:
        return ExpressionState{model_.null_type(), ValueCategory::kValue};
      case LiteralKind::kEnum:
        diagnostics_.error(range, "enum values must name a declared case");
        return ExpressionState{model_.error_type()};
    }
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_array_literal(const ArrayLiteralExpression& array,
                                        SourceRange range,
                                        std::optional<TypeId> expected) {
    std::vector<ExpressionState> elements;
    elements.reserve(array.elements.size());
    std::optional<TypeId> element_type;
    bool contains_null = false;
    bool contains_bottom = false;
    std::optional<TypeId> contextual_element;
    if (expected) {
      const SemanticType& contextual_array = model_.type(*expected);
      if (contextual_array.kind == TypeKind::kArray &&
          contextual_array.element_type && !array.elements.empty()) {
        contextual_element = contextual_array.element_type;
        element_type = contextual_element;
      }
    }
    for (const ExpressionId element : array.elements) {
      ExpressionState state = analyze_expression_with_effect_reachability(
          element, !contains_bottom, contextual_element);
      check_value(state, expression_range(element));
      contains_null = contains_null || state.type == model_.null_type();
      contains_bottom = contains_bottom || state.type == model_.bottom_type();
      if (state.type != model_.error_type() &&
          state.type != model_.bottom_type() &&
          state.type != model_.void_type() &&
          state.type != model_.null_type() && !element_type) {
        element_type = state.type;
      }
      elements.push_back(std::move(state));
    }

    if (!element_type) {
      diagnostics_.error(range,
                         "cannot infer the element type of an empty or "
                         "null-only array literal");
      return ExpressionState{model_.error_type()};
    }
    for (const ExpressionState& element : elements) {
      if (element.type == model_.error_type() ||
          element.type == model_.null_type()) {
        continue;
      }
      if (is_assignable(*element_type, element.type)) {
        continue;
      }
      if (is_assignable(element.type, *element_type)) {
        element_type = element.type;
        continue;
      }
      if (is_reference(*element_type) && is_reference(element.type)) {
        const bool nullable =
            model_.type(*element_type).kind == TypeKind::kNullable ||
            model_.type(element.type).kind == TypeKind::kNullable;
        element_type = nullable ? model_.get_nullable_type(model_.object_type())
                                : model_.object_type();
      }
    }
    if (contains_null && is_reference(*element_type)) {
      if (model_.type(*element_type).kind != TypeKind::kNullable) {
        element_type = model_.get_nullable_type(*element_type);
      }
    }
    for (std::size_t index = 0; index < elements.size(); ++index) {
      check_assignment(*element_type, elements[index].type,
                       expression_range(array.elements[index]),
                       "array element");
    }
    if (contains_bottom) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    return ExpressionState{model_.get_array_type(*element_type),
                           ValueCategory::kValue};
  }

  ExpressionState analyze_index(const IndexExpression& index,
                                SourceRange range) {
    const ExpressionState object = analyze_expression(index.object);
    const ExpressionState subscript =
        analyze_expression_with_effect_reachability(
            index.index, object.type != model_.bottom_type());
    check_value(object, expression_range(index.object));
    check_value(subscript, expression_range(index.index));
    if (object.type == model_.bottom_type() ||
        subscript.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    if (subscript.type != model_.error_type()) {
      check_assignment(*model_.find_type("int32"), subscript.type,
                       expression_range(index.index), "array index");
    }
    if (object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (object.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    const SemanticType& type = model_.type(object.type);
    if (type.kind == TypeKind::kNullable && type.element_type &&
        model_.type(*type.element_type).kind == TypeKind::kArray) {
      diagnostics_.error(range, "nullable array type '" + type.name +
                                    "' cannot be indexed without narrowing");
      return ExpressionState{model_.error_type()};
    }
    if (type.kind != TypeKind::kArray || !type.element_type) {
      diagnostics_.error(range, "type '" + type.name + "' cannot be indexed");
      return ExpressionState{model_.error_type()};
    }
    return ExpressionState{*type.element_type, ValueCategory::kMutableLocation};
  }

  ExpressionState analyze_unary_chain(ExpressionId root,
                                      std::optional<TypeId> expected) {
    std::vector<ExpressionId> unary_ids;
    ExpressionId current = root;
    std::optional<TypeId> operand_expected = expected;
    bool is_negated_literal = false;
    while (
        const auto* unary = std::get_if<UnaryExpression>(
            &files_[current_file_.value]->storage.expression(current).data)) {
      unary_ids.push_back(current);
      const bool numeric_sign = unary->operation == TokenKind::kPlus ||
                                unary->operation == TokenKind::kMinus;
      operand_expected = numeric_sign ? operand_expected : std::nullopt;
      is_negated_literal = unary->operation == TokenKind::kMinus;
      current = unary->operand;
    }

    ExpressionState state =
        analyze_expression(current, operand_expected, is_negated_literal);
    for (auto entry = unary_ids.rbegin(); entry != unary_ids.rend(); ++entry) {
      const Expression& expression =
          files_[current_file_.value]->storage.expression(*entry);
      const auto& unary = std::get<UnaryExpression>(expression.data);
      state =
          analyze_unary_with_operand(unary, expression.range, std::move(state));
      if (*entry != root) {
        state = record_expression(*entry, std::move(state));
      }
    }
    return state;
  }

  ExpressionState analyze_unary_with_operand(const UnaryExpression& unary,
                                             SourceRange range,
                                             ExpressionState operand) {
    if (!check_value(operand, expression_range(unary.operand)) ||
        operand.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (operand.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    if (unary.operation == TokenKind::kBang) {
      if (operand.type != model_.bool_type() &&
          !mark_presence_test(operand, unary.operand)) {
        report_operator_type(unary.operation, range, operand.type);
        return ExpressionState{model_.error_type()};
      }
      return ExpressionState{model_.bool_type(), ValueCategory::kValue};
    }
    if ((unary.operation == TokenKind::kPlus ||
         unary.operation == TokenKind::kMinus) &&
        is_numeric(operand.type)) {
      return ExpressionState{operand.type, ValueCategory::kValue};
    }
    if (unary.operation == TokenKind::kTilde && is_integer(operand.type)) {
      return ExpressionState{operand.type, ValueCategory::kValue};
    }
    report_operator_type(unary.operation, range, operand.type);
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_update(const UpdateExpression& update,
                                 SourceRange range) {
    ExpressionState operand = analyze_expression(update.operand);
    if (operand.symbol && operand.category == ValueCategory::kMutableLocation) {
      const SemanticSymbol& symbol = model_.symbol(*operand.symbol);
      if (symbol.kind == SymbolKind::kLocal ||
          symbol.kind == SymbolKind::kParameter) {
        operand.type = symbol.type;
        restore_assignment_target_type(update.operand, symbol.type);
      }
    }
    if (operand.type != model_.error_type() &&
        operand.category != ValueCategory::kMutableLocation) {
      diagnostics_.error(expression_range(update.operand),
                         "update target is not mutable");
    }
    if (operand.symbol && model_.symbol(*operand.symbol).is_final) {
      const SemanticSymbol& symbol = model_.symbol(*operand.symbol);
      diagnostics_.error(expression_range(update.operand),
                         "cannot update final " +
                             std::string{symbol_kind_name(symbol.kind)} + " '" +
                             symbol.name + "'");
    }
    if (!check_value(operand, expression_range(update.operand)) ||
        operand.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (!is_numeric(operand.type)) {
      report_operator_type(update.operation, range, operand.type);
      return ExpressionState{model_.error_type()};
    }
    if (operand.symbol) {
      invalidate_narrowing(*operand.symbol);
    }
    return ExpressionState{
        operand.type, ValueCategory::kValue, operand.symbol, {}};
  }

  ExpressionState analyze_binary_chain(ExpressionId root,
                                       std::optional<TypeId> expected) {
    const std::optional<TypeId> numeric_context =
        expected && is_numeric(*expected) ? expected : std::nullopt;
    std::vector<ExpressionId> binary_ids;
    ExpressionId current = root;
    while (
        const auto* binary = std::get_if<BinaryExpression>(
            &files_[current_file_.value]->storage.expression(current).data)) {
      binary_ids.push_back(current);
      current = binary->left;
    }

    ExpressionState state = analyze_expression(current, numeric_context);
    for (auto entry = binary_ids.rbegin(); entry != binary_ids.rend();
         ++entry) {
      const Expression& expression =
          files_[current_file_.value]->storage.expression(*entry);
      const auto& binary = std::get<BinaryExpression>(expression.data);
      state = analyze_binary_with_left(binary, expression.range,
                                       numeric_context, std::move(state));
      if (*entry != root) {
        state = record_expression(*entry, std::move(state));
      }
    }
    return state;
  }

  ExpressionState analyze_binary_with_left(const BinaryExpression& binary,
                                           SourceRange range,
                                           std::optional<TypeId> expected,
                                           ExpressionState left) {
    const bool is_shift = binary.operation == TokenKind::kShiftLeft ||
                          binary.operation == TokenKind::kShiftRight;
    const std::optional<TypeId> numeric_context =
        expected && is_numeric(*expected) ? expected : std::nullopt;
    const bool is_short_circuit =
        binary.operation == TokenKind::kAmpersandAmpersand ||
        binary.operation == TokenKind::kPipePipe;
    const bool left_is_boolean =
        left.type == model_.bool_type() ||
        (is_short_circuit && mark_presence_test(left, binary.left));
    const NonNullSet right_base = active_non_null_;
    if (is_short_circuit && !constant_context_) {
      const ConditionFacts facts = condition_facts(binary.left);
      add_non_null_facts(binary.operation == TokenKind::kAmpersandAmpersand
                             ? facts.when_true
                             : facts.when_false);
    }
    const std::optional<TypeId> right_context =
        is_shift ? std::nullopt
        : numeric_context
            ? numeric_context
            : (is_numeric(left.type) ? std::optional<TypeId>{left.type}
                                     : std::nullopt);
    ExpressionState right = analyze_expression_with_effect_reachability(
        binary.right, left.type != model_.bottom_type(), right_context);
    const bool right_is_boolean =
        right.type == model_.bool_type() ||
        (is_short_circuit && mark_presence_test(right, binary.right));
    if (is_short_circuit && !constant_context_) {
      active_non_null_ = intersect_symbols(right_base, active_non_null_);
    }
    return finish_binary(binary, range, left, right, left_is_boolean,
                         right_is_boolean);
  }

  ExpressionState finish_binary(const BinaryExpression& binary,
                                SourceRange range, ExpressionState& left,
                                ExpressionState& right, bool left_is_boolean,
                                bool right_is_boolean) {
    const bool values = check_value(left, expression_range(binary.left)) &&
                        check_value(right, expression_range(binary.right));
    if (!values || left.type == model_.error_type() ||
        right.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (left.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    if (right.type == model_.bottom_type()) {
      if (binary.operation == TokenKind::kAmpersandAmpersand ||
          binary.operation == TokenKind::kPipePipe) {
        return left_is_boolean
                   ? ExpressionState{model_.bool_type(), ValueCategory::kValue}
                   : ExpressionState{model_.error_type()};
      }
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }

    switch (binary.operation) {
      case TokenKind::kPlus:
        if (left.type == model_.string_type() &&
            right.type == model_.string_type()) {
          return ExpressionState{model_.string_type(), ValueCategory::kValue};
        }
        [[fallthrough]];
      case TokenKind::kMinus:
      case TokenKind::kStar:
      case TokenKind::kSlash:
        if (const std::optional<TypeId> common =
                common_binary_numeric_type(binary, left, right)) {
          ExpressionState result{*common, ValueCategory::kValue};
          if (binary.operation == TokenKind::kSlash && is_integer(*common) &&
              !integer_divisor_is_proven_nonzero(binary.right)) {
            result.may_divide_by_zero = true;
            if (!constant_context_) {
              record_direct_error(model_.division_by_zero_type(), range);
            }
          }
          return result;
        }
        break;
      case TokenKind::kPercent:
        if (const std::optional<TypeId> common =
                common_binary_numeric_type(binary, left, right);
            common && is_integer(*common)) {
          ExpressionState result{*common, ValueCategory::kValue};
          if (!integer_divisor_is_proven_nonzero(binary.right)) {
            result.may_divide_by_zero = true;
            if (!constant_context_) {
              record_direct_error(model_.division_by_zero_type(), range);
            }
          }
          return result;
        }
        break;
      case TokenKind::kAmpersand:
      case TokenKind::kPipe:
      case TokenKind::kCaret:
        if (const std::optional<TypeId> common =
                common_binary_numeric_type(binary, left, right);
            common && is_integer(*common)) {
          return ExpressionState{*common, ValueCategory::kValue};
        }
        break;
      case TokenKind::kShiftLeft:
      case TokenKind::kShiftRight:
        if (is_integer(left.type) && is_integer(right.type)) {
          if (!check_shift_literal(binary.right, left.type)) {
            return ExpressionState{model_.error_type()};
          }
          return ExpressionState{left.type, ValueCategory::kValue};
        }
        break;
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
        if (const std::optional<TypeId> common =
                common_binary_numeric_type(binary, left, right)) {
          return ExpressionState{model_.bool_type(), ValueCategory::kValue};
        }
        break;
      case TokenKind::kEqualEqual:
      case TokenKind::kBangEqual:
        if (common_binary_numeric_type(binary, left, right) ||
            is_assignable(left.type, right.type) ||
            is_assignable(right.type, left.type) ||
            is_declared_nullable_null_comparison(binary)) {
          return ExpressionState{model_.bool_type(), ValueCategory::kValue};
        }
        break;
      case TokenKind::kAmpersandAmpersand:
      case TokenKind::kPipePipe:
        if (left_is_boolean && right_is_boolean) {
          return ExpressionState{model_.bool_type(), ValueCategory::kValue};
        }
        break;
      default:
        break;
    }

    diagnostics_.error(
        range, "operator '" + std::string{token_kind_name(binary.operation)} +
                   "' cannot be applied to '" + type_name(left.type) +
                   "' and '" + type_name(right.type) + "'");
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_type_test(const TypeTestExpression& test,
                                    SourceRange range) {
    const ExpressionState value = analyze_expression(test.value);
    check_value(value, expression_range(test.value));
    const TypeId target = resolve_type(test.target, current_file_);
    if (value.type == model_.error_type() || target == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (value.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    if (model_.type(target).kind == TypeKind::kNullable) {
      diagnostics_.error(test.target.range,
                         "the right operand of 'is' must be non-nullable");
      return ExpressionState{model_.error_type()};
    }
    if (!check_runtime_type_operation(value.type, target, range)) {
      return ExpressionState{model_.error_type()};
    }
    return ExpressionState{
        model_.bool_type(), ValueCategory::kValue, {}, {}, target};
  }

  ExpressionState analyze_checked_cast(const CheckedCastExpression& cast,
                                       SourceRange range) {
    const ExpressionState value = analyze_expression(cast.value);
    check_value(value, expression_range(cast.value));
    const TypeId target = resolve_type(cast.target, current_file_);
    if (value.type == model_.error_type() || target == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (value.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    const SemanticType& target_type = model_.type(target);
    if (target_type.kind != TypeKind::kNullable || !target_type.element_type) {
      diagnostics_.error(cast.target.range,
                         "the right operand of 'as' must be nullable");
      return ExpressionState{model_.error_type()};
    }
    if (!check_runtime_type_operation(value.type, *target_type.element_type,
                                      range)) {
      return ExpressionState{model_.error_type()};
    }
    return ExpressionState{
        target, ValueCategory::kValue, {}, {}, *target_type.element_type};
  }

  ExpressionState analyze_numeric_conversion(
      const NumericConversionExpression& conversion, SourceRange range) {
    const TypeId target = resolve_type(conversion.target, current_file_);
    if (target == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (!is_numeric(target)) {
      diagnostics_.error(conversion.target.range,
                         "numeric conversion target must be numeric");
      return ExpressionState{model_.error_type()};
    }

    const bool is_literal = is_numeric_literal_expression(conversion.value);
    const bool is_contextual_literal =
        is_contextual_numeric_literal_expression(conversion.value);
    const ExpressionState value =
        is_contextual_literal ? analyze_overload_argument(conversion.value)
                              : analyze_expression(conversion.value);
    return finish_numeric_conversion(conversion, range, target, value,
                                     is_literal, is_contextual_literal);
  }

  ExpressionState finish_numeric_conversion(
      const NumericConversionExpression& conversion, SourceRange range,
      TypeId target, const ExpressionState& value, bool is_literal,
      bool is_contextual_literal) {
    check_value(value, expression_range(conversion.value));
    if (value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (value.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    if (!is_numeric(value.type)) {
      diagnostics_.error(
          range, "numeric conversion requires a numeric value; found '" +
                     type_name(value.type) + "'");
      return ExpressionState{model_.error_type()};
    }

    if (is_literal) {
      bool fits = false;
      if (is_contextual_literal) {
        fits = numeric_literal_conversion_fits(conversion.value, target);
      } else if (const auto constant =
                     numeric_literal_expression_constant(conversion.value)) {
        fits = convert_scalar(constant->bits, model_.type(constant->type).kind,
                              model_.type(target).kind)
                   .has_value();
      }
      if (!fits) {
        diagnostics_.error(
            range,
            "numeric literal is out of range for explicit conversion "
            "to '" +
                type_name(target) + "'");
        return ExpressionState{model_.error_type()};
      }
      if (is_contextual_literal) {
        static_cast<void>(
            contextualize_numeric_conversion(conversion.value, target));
      }
    }
    return ExpressionState{target, ValueCategory::kValue};
  }

  ExpressionState analyze_integer_conversion(
      const IntegerConversionExpression& conversion, SourceRange range) {
    const TypeId target = resolve_type(conversion.target, current_file_);
    const ExpressionState value = analyze_expression(conversion.value);
    check_value(value, expression_range(conversion.value));
    if (target == model_.error_type() || value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (value.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }

    bool valid = true;
    if (!is_integer(target)) {
      diagnostics_.error(
          conversion.target.range,
          "integer conversion target must be an integer; found '" +
              type_name(target) + "'");
      valid = false;
    }
    if (conversion.operation != "wrap" && conversion.operation != "sat") {
      diagnostics_.error(range, "integer type '" + type_name(target) +
                                    "' has no conversion mode '" +
                                    std::string{conversion.operation} + "'");
      valid = false;
    }
    if (!is_integer(value.type)) {
      diagnostics_.error(range, std::string{conversion.target.name} +
                                    "::" + std::string{conversion.operation} +
                                    " requires an integer value; found '" +
                                    type_name(value.type) + "'");
      valid = false;
    }
    return ExpressionState{valid ? target : model_.error_type(),
                           ValueCategory::kValue};
  }

  bool check_runtime_type_operation(TypeId source, TypeId target,
                                    SourceRange range) {
    TypeId source_base = source;
    const SemanticType& source_type = model_.type(source);
    if (source_type.kind == TypeKind::kNullable && source_type.element_type) {
      source_base = *source_type.element_type;
    }
    const TypeKind source_kind = model_.type(source_base).kind;
    const TypeKind target_kind = model_.type(target).kind;
    if (!is_reference(source) && source != model_.null_type()) {
      diagnostics_.error(range,
                         "checked type operations require a managed "
                         "reference; found '" +
                             type_name(source) + "'");
      return false;
    }
    if (target_kind == TypeKind::kArray) {
      diagnostics_.error(
          range, "checked array casts require reified array type metadata");
      return false;
    }
    if (target_kind != TypeKind::kObject && target_kind != TypeKind::kString &&
        target_kind != TypeKind::kFileClass &&
        target_kind != TypeKind::kErrorClass &&
        target_kind != TypeKind::kInterface) {
      diagnostics_.error(
          range, "type '" + type_name(target) + "' is not runtime-checkable");
      return false;
    }
    if (source == model_.null_type() || source_kind == TypeKind::kObject ||
        target_kind == TypeKind::kObject || source_base == target) {
      return true;
    }
    if ((source_kind == TypeKind::kFileClass ||
         source_kind == TypeKind::kErrorClass) &&
        (target_kind == TypeKind::kFileClass ||
         target_kind == TypeKind::kErrorClass) &&
        (is_file_class_subtype(source_base, target) ||
         is_file_class_subtype(target, source_base))) {
      return true;
    }
    if (source_kind == TypeKind::kInterface &&
        target_kind == TypeKind::kInterface) {
      return true;
    }
    if ((source_kind == TypeKind::kFileClass ||
         source_kind == TypeKind::kErrorClass) &&
        target_kind == TypeKind::kInterface) {
      const SemanticType& source_info = model_.type(source_base);
      if (implements_interface(source_base, target) ||
          (source_info.file && !model_.file(*source_info.file).is_sealed)) {
        return true;
      }
    }
    if (source_kind == TypeKind::kInterface &&
        (target_kind == TypeKind::kFileClass ||
         target_kind == TypeKind::kErrorClass)) {
      const SemanticType& target_info = model_.type(target);
      if (implements_interface(target, source_base) ||
          (target_info.file && !model_.file(*target_info.file).is_sealed)) {
        return true;
      }
    }
    diagnostics_.error(range, "types '" + type_name(source) + "' and '" +
                                  type_name(target) +
                                  "' cannot overlap at runtime");
    return false;
  }

  ExpressionState analyze_assignment(const AssignmentExpression& assignment,
                                     SourceRange range) {
    ExpressionState target = analyze_expression(assignment.target);
    if (target.symbol && target.category == ValueCategory::kMutableLocation) {
      const SemanticSymbol& symbol = model_.symbol(*target.symbol);
      if (symbol.kind == SymbolKind::kLocal ||
          symbol.kind == SymbolKind::kParameter) {
        target.type = symbol.type;
        restore_assignment_target_type(assignment.target, symbol.type);
      }
    }
    const bool is_shift_assignment =
        assignment.operation == TokenKind::kShiftLeftEqual ||
        assignment.operation == TokenKind::kShiftRightEqual;
    const ExpressionState value = analyze_expression_with_effect_reachability(
        assignment.value, target.type != model_.bottom_type(),
        is_shift_assignment ? std::nullopt : std::optional{target.type});
    if (target.type != model_.error_type() &&
        target.category != ValueCategory::kMutableLocation &&
        !(target.symbol && assignment.operation == TokenKind::kEqual &&
          can_initialize_final_field(assignment.target, *target.symbol))) {
      diagnostics_.error(expression_range(assignment.target),
                         "assignment target is not mutable");
    }
    if (target.symbol &&
        model_.symbol(*target.symbol).kind == SymbolKind::kSelf) {
      diagnostics_.error(
          expression_range(assignment.target),
          "cannot replace 'self'; initialize its fields instead");
    }
    if (target.symbol && model_.symbol(*target.symbol).is_final &&
        (assignment.operation != TokenKind::kEqual ||
         !can_initialize_final_field(assignment.target, *target.symbol))) {
      const SemanticSymbol& symbol = model_.symbol(*target.symbol);
      diagnostics_.error(
          expression_range(assignment.target),
          (assignment.operation == TokenKind::kEqual ? "cannot assign to final "
                                                     : "cannot update final ") +
              std::string{symbol_kind_name(symbol.kind)} + " '" + symbol.name +
              "'");
    }
    if (assignment.operation != TokenKind::kEqual) {
      check_value(target, expression_range(assignment.target));
    }
    check_value(value, expression_range(assignment.value));
    if (value.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    if (target.symbol) {
      invalidate_narrowing(*target.symbol);
    }
    if (target.type == model_.error_type() ||
        value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }

    bool may_divide_by_zero = false;
    if (assignment.operation == TokenKind::kEqual) {
      check_assignment(target.type, value.type, range, "assignment");
    } else {
      bool is_valid = false;
      switch (assignment.operation) {
        case TokenKind::kPlusEqual:
          is_valid = (target.type == model_.string_type() &&
                      value.type == model_.string_type()) ||
                     (is_numeric(target.type) && is_numeric(value.type) &&
                      is_assignable(target.type, value.type));
          break;
        case TokenKind::kMinusEqual:
        case TokenKind::kStarEqual:
        case TokenKind::kSlashEqual:
          is_valid = is_numeric(target.type) && is_numeric(value.type) &&
                     is_assignable(target.type, value.type);
          break;
        case TokenKind::kPercentEqual:
          is_valid = is_integer(target.type) && is_integer(value.type) &&
                     is_assignable(target.type, value.type);
          break;
        case TokenKind::kAmpersandEqual:
        case TokenKind::kPipeEqual:
        case TokenKind::kCaretEqual:
          is_valid = is_integer(target.type) && is_integer(value.type) &&
                     is_assignable(target.type, value.type);
          break;
        case TokenKind::kShiftLeftEqual:
        case TokenKind::kShiftRightEqual:
          is_valid = is_integer(target.type) && is_integer(value.type) &&
                     check_shift_literal(assignment.value, target.type);
          break;
        default:
          break;
      }
      if (!is_valid) {
        diagnostics_.error(
            range, "operator '" +
                       std::string{token_kind_name(assignment.operation)} +
                       "' cannot be applied to '" + type_name(target.type) +
                       "' and '" + type_name(value.type) + "'");
        return ExpressionState{model_.error_type()};
      }
      if ((assignment.operation == TokenKind::kSlashEqual ||
           assignment.operation == TokenKind::kPercentEqual) &&
          is_integer(target.type) &&
          !integer_divisor_is_proven_nonzero(assignment.value)) {
        may_divide_by_zero = true;
        if (!constant_context_) {
          record_direct_error(model_.division_by_zero_type(), range);
        }
      }
    }
    ExpressionState result{
        target.type, ValueCategory::kValue, target.symbol, {}};
    result.may_divide_by_zero = may_divide_by_zero;
    return result;
  }

  std::optional<TypeId> narrowed_type(SymbolId symbol) const {
    if (!contains_symbol(active_non_null_, symbol)) {
      return std::nullopt;
    }
    const SemanticType& declared = model_.type(model_.symbol(symbol).type);
    if (declared.kind != TypeKind::kNullable || !declared.element_type) {
      return std::nullopt;
    }
    return declared.element_type;
  }

  void add_non_null_facts(const NonNullSet& facts) {
    for (const SymbolId symbol : facts) {
      const SemanticSymbol& declared = model_.symbol(symbol);
      const TypeKind kind = model_.type(declared.type).kind;
      if ((declared.kind == SymbolKind::kLocal ||
           declared.kind == SymbolKind::kParameter) &&
          kind == TypeKind::kNullable) {
        add_symbol(active_non_null_, symbol);
      }
    }
  }

  void invalidate_narrowing(SymbolId symbol) {
    std::erase(active_non_null_, symbol);
  }

  ConditionFacts condition_facts(ExpressionId id) const {
    const ExpressionSemantics& semantics =
        model_.file(current_file_).expressions.at(id.value);
    if (semantics.type != model_.bool_type() && !semantics.is_presence_test) {
      return {};
    }

    ConditionFacts facts = raw_condition_facts(id);
    const auto remove_assigned = [this, id](NonNullSet& symbols) {
      std::erase_if(symbols, [this, id](SymbolId symbol) {
        return expression_assigns_symbol(id, symbol);
      });
    };
    remove_assigned(facts.when_true);
    remove_assigned(facts.when_false);
    return facts;
  }

  ConditionFacts raw_condition_facts(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    const ExpressionSemantics& semantics =
        model_.file(current_file_).expressions.at(id.value);
    if (semantics.is_presence_test) {
      ConditionFacts facts;
      if (const std::optional<SymbolId> symbol = narrowable_symbol(id)) {
        facts.when_true.push_back(*symbol);
      }
      return facts;
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return condition_facts(grouped->expression);
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data)) {
      if (unary->operation != TokenKind::kBang) {
        return {};
      }
      ConditionFacts facts = condition_facts(unary->operand);
      std::swap(facts.when_true, facts.when_false);
      return facts;
    }
    const auto* binary = std::get_if<BinaryExpression>(&expression.data);
    if (binary == nullptr) {
      return {};
    }

    if (binary->operation == TokenKind::kEqualEqual ||
        binary->operation == TokenKind::kBangEqual) {
      std::optional<SymbolId> symbol;
      if (is_null_expression(binary->right)) {
        symbol = narrowable_symbol(binary->left);
      } else if (is_null_expression(binary->left)) {
        symbol = narrowable_symbol(binary->right);
      }
      if (!symbol) {
        return {};
      }
      ConditionFacts facts;
      NonNullSet& destination = binary->operation == TokenKind::kBangEqual
                                    ? facts.when_true
                                    : facts.when_false;
      destination.push_back(*symbol);
      return facts;
    }

    if (binary->operation != TokenKind::kAmpersandAmpersand &&
        binary->operation != TokenKind::kPipePipe) {
      return {};
    }
    const ConditionFacts left = condition_facts(binary->left);
    const ConditionFacts right = condition_facts(binary->right);
    ConditionFacts result;
    if (binary->operation == TokenKind::kAmpersandAmpersand) {
      result.when_true = union_symbols(left.when_true, right.when_true);
      result.when_false = intersect_symbols(
          left.when_false, union_symbols(left.when_true, right.when_false));
    } else {
      result.when_false = union_symbols(left.when_false, right.when_false);
      result.when_true = intersect_symbols(
          left.when_true, union_symbols(left.when_false, right.when_true));
    }
    return result;
  }

  std::optional<SymbolId> narrowable_symbol(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return narrowable_symbol(grouped->expression);
    }
    if (!std::holds_alternative<IdentifierExpression>(expression.data)) {
      return std::nullopt;
    }
    const ExpressionSemantics& semantics =
        model_.file(current_file_).expressions.at(id.value);
    if (!semantics.symbol) {
      return std::nullopt;
    }
    const SemanticSymbol& symbol = model_.symbol(*semantics.symbol);
    if ((symbol.kind != SymbolKind::kLocal &&
         symbol.kind != SymbolKind::kParameter) ||
        model_.type(symbol.type).kind != TypeKind::kNullable) {
      return std::nullopt;
    }
    return semantics.symbol;
  }

  bool is_null_expression(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return is_null_expression(grouped->expression);
    }
    const auto* literal = std::get_if<LiteralExpression>(&expression.data);
    return literal != nullptr && literal->kind == LiteralKind::kNull;
  }

  bool expression_assigns_symbol(ExpressionId id, SymbolId symbol) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data)) {
      return expression_assigns_symbol(unary->operand, symbol);
    }
    if (const auto* update = std::get_if<UpdateExpression>(&expression.data)) {
      return narrowable_symbol(update->operand) == symbol ||
             expression_assigns_symbol(update->operand, symbol);
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.data)) {
      return expression_assigns_symbol(binary->left, symbol) ||
             expression_assigns_symbol(binary->right, symbol);
    }
    if (const auto* test = std::get_if<TypeTestExpression>(&expression.data)) {
      return expression_assigns_symbol(test->value, symbol);
    }
    if (const auto* cast =
            std::get_if<CheckedCastExpression>(&expression.data)) {
      return expression_assigns_symbol(cast->value, symbol);
    }
    if (const auto* conversion =
            std::get_if<NumericConversionExpression>(&expression.data)) {
      return expression_assigns_symbol(conversion->value, symbol);
    }
    if (const auto* conversion =
            std::get_if<IntegerConversionExpression>(&expression.data)) {
      return expression_assigns_symbol(conversion->value, symbol);
    }
    if (const auto* assignment =
            std::get_if<AssignmentExpression>(&expression.data)) {
      return narrowable_symbol(assignment->target) == symbol ||
             expression_assigns_symbol(assignment->target, symbol) ||
             expression_assigns_symbol(assignment->value, symbol);
    }
    if (const auto* member =
            std::get_if<MemberAccessExpression>(&expression.data)) {
      return expression_assigns_symbol(member->object, symbol);
    }
    if (const auto* member =
            std::get_if<SafeMemberAccessExpression>(&expression.data)) {
      return expression_assigns_symbol(member->object, symbol);
    }
    if (const auto* meta =
            std::get_if<MetaAccessExpression>(&expression.data)) {
      return expression_assigns_symbol(meta->object, symbol);
    }
    if (const auto* coalesce =
            std::get_if<NullCoalesceExpression>(&expression.data)) {
      return expression_assigns_symbol(coalesce->nullable, symbol) ||
             expression_assigns_symbol(coalesce->fallback, symbol);
    }
    if (const auto* assertion =
            std::get_if<NullAssertExpression>(&expression.data)) {
      return expression_assigns_symbol(assertion->operand, symbol);
    }
    if (const auto* call = std::get_if<CallExpression>(&expression.data)) {
      if (expression_assigns_symbol(call->callee, symbol)) {
        return true;
      }
      for (const ExpressionId argument : call->arguments) {
        if (expression_assigns_symbol(argument, symbol)) {
          return true;
        }
      }
      return false;
    }
    if (const auto* array =
            std::get_if<ArrayLiteralExpression>(&expression.data)) {
      for (const ExpressionId element : array->elements) {
        if (expression_assigns_symbol(element, symbol)) {
          return true;
        }
      }
      return false;
    }
    if (const auto* index = std::get_if<IndexExpression>(&expression.data)) {
      return expression_assigns_symbol(index->object, symbol) ||
             expression_assigns_symbol(index->index, symbol);
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return expression_assigns_symbol(grouped->expression, symbol);
    }
    return false;
  }

  bool is_declared_nullable_null_comparison(
      const BinaryExpression& binary) const {
    return (is_null_expression(binary.left) &&
            narrowable_symbol(binary.right).has_value()) ||
           (is_null_expression(binary.right) &&
            narrowable_symbol(binary.left).has_value());
  }

  void restore_assignment_target_type(ExpressionId id, TypeId type) {
    model_.mutable_file(current_file_).expressions.at(id.value).type = type;
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      restore_assignment_target_type(grouped->expression, type);
    }
  }

  ValueCategory self_category() const {
    if (model_.file(current_file_).kind != FileTypeKind::kStruct) {
      return ValueCategory::kValue;
    }
    return current_callable_kind_ == SymbolKind::kConstructor
               ? ValueCategory::kMutableLocation
               : ValueCategory::kReadOnlyLocation;
  }

  ValueCategory field_category(const SemanticSymbol& field,
                               ValueCategory receiver) const {
    // A final aggregate is read-only even within a constructor: only direct
    // whole-field initialization is allowed, not writes through its subfields.
    if (field.is_final && model_.type(field.type).kind == TypeKind::kStruct) {
      return ValueCategory::kReadOnlyLocation;
    }
    if (!field.is_static && field.file &&
        model_.file(*field.file).kind == FileTypeKind::kStruct) {
      return receiver == ValueCategory::kMutableLocation ||
                     receiver == ValueCategory::kReadOnlyLocation
                 ? receiver
                 : ValueCategory::kValue;
    }
    // A managed reference breaks the inline read-only path.
    return ValueCategory::kMutableLocation;
  }

  bool can_initialize_final_field(ExpressionId target, SymbolId symbol) const {
    const SemanticSymbol& target_symbol = model_.symbol(symbol);
    return current_callable_kind_ == SymbolKind::kConstructor &&
           target_symbol.kind == SymbolKind::kField &&
           !target_symbol.is_static && target_symbol.file == current_file_ &&
           is_self_field_reference(target, symbol);
  }

  bool is_self_field_reference(ExpressionId id, SymbolId symbol) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (std::holds_alternative<IdentifierExpression>(expression.data)) {
      return true;
    }
    if (const auto* member =
            std::get_if<MemberAccessExpression>(&expression.data)) {
      const ExpressionSemantics& object =
          model_.file(current_file_).expressions.at(member->object.value);
      return object.symbol == model_.file(current_file_).self_symbol;
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return is_self_field_reference(grouped->expression, symbol);
    }
    return false;
  }

  ExpressionState analyze_member_access(const MemberAccessExpression& member,
                                        SourceRange range) {
    const ExpressionState object = analyze_expression(member.object);
    if (object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (object.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    const SemanticType& object_type = model_.type(object.type);
    if (object_type.kind == TypeKind::kEnum && object_type.file) {
      if (object.category != ValueCategory::kType) {
        diagnostics_.error(range,
                           "enum cases must be accessed through their type");
        return ExpressionState{model_.error_type()};
      }
      const auto& names = enum_case_names_[object_type.file->value];
      const auto found = names.find(member.member);
      if (found != names.end()) {
        return ExpressionState{object.type, ValueCategory::kValue,
                               found->second};
      }
      diagnostics_.error(range, "enum '" + object_type.name +
                                    "' has no case '" +
                                    std::string{member.member} + "'");
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kNullable) {
      diagnostics_.error(range, "nullable type '" + object_type.name +
                                    "' has no members without narrowing");
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kArray) {
      if (member.member == "Length") {
        diagnostics_.error(range,
                           "array length is a meta query; use '::length'");
      } else {
        diagnostics_.error(range, "array type '" + object_type.name +
                                      "' has no member '" +
                                      std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kString) {
      if (member.member == "Length") {
        diagnostics_.error(range,
                           "string length is a meta query; use '::length'");
      } else if (member.member == "ByteLength") {
        diagnostics_.error(
            range, "string byte length is a meta query; use '::byteLength'");
      } else if (member.member == "IsEmpty") {
        diagnostics_.error(range,
                           "string emptiness is a meta query; use '::isEmpty'");
      } else {
        diagnostics_.error(
            range, "string has no member '" + std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kErrorClass && !object_type.file &&
        member.member == "Message") {
      return ExpressionState{
          model_.string_type(),
          field_category(model_.symbol(error_message_symbol_), object.category),
          error_message_symbol_,
          {}};
    }
    if ((object_type.kind != TypeKind::kFileClass &&
         object_type.kind != TypeKind::kErrorClass &&
         object_type.kind != TypeKind::kInterface &&
         object_type.kind != TypeKind::kStruct) ||
        !object_type.file) {
      diagnostics_.error(
          range, "type '" + object_type.name + "' has no Cloth members");
      return ExpressionState{model_.error_type()};
    }

    const FileId target_file = *object_type.file;
    std::vector<SymbolId> members = find_members(target_file, member.member);
    if (members.empty()) {
      if (has_inaccessible_member(target_file, member.member)) {
        diagnostics_.error(range, "member '" + std::string{member.member} +
                                      "' is private in file class '" +
                                      object_type.name + "'");
      } else {
        diagnostics_.error(range, "type '" + object_type.name +
                                      "' has no member '" +
                                      std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }

    const SemanticSymbol& first = model_.symbol(members.front());
    if (first.kind == SymbolKind::kField) {
      if (object.category == ValueCategory::kSuper) {
        diagnostics_.error(
            range, "'super' may qualify only an instance function call");
        return ExpressionState{model_.error_type()};
      }
      if (first.is_static && object.category != ValueCategory::kType) {
        diagnostics_.error(range,
                           "static field '" + first.name +
                               "' must be accessed through file class '" +
                               object_type.name + "'");
        return ExpressionState{model_.error_type()};
      }
      if (!first.is_static && object.category == ValueCategory::kType) {
        diagnostics_.error(range,
                           "field '" + first.name + "' requires an instance");
        return ExpressionState{model_.error_type()};
      }
      return ExpressionState{first.type,
                             field_category(first, object.category),
                             members.front(),
                             {}};
    }
    ExpressionState state{
        model_.error_type(), ValueCategory::kCallable, {}, std::move(members)};
    if (object_type.kind == TypeKind::kInterface) {
      state.interface_dispatch = target_file;
    }
    return state;
  }

  ExpressionState analyze_meta_access(const MetaAccessExpression& meta,
                                      SourceRange range) {
    const ExpressionState object = analyze_expression(meta.object);
    if (object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (object.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }

    const SemanticType& object_type = model_.type(object.type);
    if (object.category == ValueCategory::kType) {
      if (meta.meta != "parse") {
        diagnostics_.error(range, "type '" + object_type.name +
                                      "' has no meta operation '" +
                                      std::string{meta.meta} + "'");
        return ExpressionState{model_.error_type()};
      }
      if (!is_primitive_parse_type(object_type.kind)) {
        diagnostics_.error(range,
                           "type '" + object_type.name + "' cannot be parsed");
        return ExpressionState{model_.error_type()};
      }
      std::vector<SymbolId> candidates;
      for (std::size_t index = 0; index < model_.symbols().size(); ++index) {
        const SemanticSymbol& symbol = model_.symbol(SymbolId{index});
        if (symbol.intrinsic == IntrinsicKind::kPrimitiveParse &&
            symbol.type == object.type) {
          candidates.push_back(SymbolId{index});
        }
      }
      if (candidates.empty()) {
        diagnostics_.error(range,
                           "primitive parsing requires the compiler-paired "
                           "'cloth.lang.errors.ParseError'");
        return ExpressionState{model_.error_type()};
      }
      return ExpressionState{model_.error_type(),
                             ValueCategory::kCallable,
                             {},
                             std::move(candidates)};
    }
    if (!check_value(object, expression_range(meta.object))) {
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kNullable) {
      diagnostics_.error(range, "nullable type '" + object_type.name +
                                    "' has no meta queries without narrowing");
      return ExpressionState{model_.error_type()};
    }
    if (meta.meta == "typeName" &&
        (is_reference(object.type) || object_type.kind == TypeKind::kEnum ||
         object_type.kind == TypeKind::kStruct)) {
      return ExpressionState{model_.string_type(), ValueCategory::kValue};
    }
    if (is_integer(object.type) &&
        (meta.meta == "writeLittleEndian" || meta.meta == "writeBigEndian")) {
      ExpressionState state{model_.error_type(), ValueCategory::kCallable};
      state.integer_meta_operation = IntegerMetaOperation{
          IntegerMetaOperationKind::kWrite,
          meta.meta == "writeLittleEndian" ? IntegerByteOrder::kLittleEndian
                                           : IntegerByteOrder::kBigEndian,
          object.type};
      return state;
    }
    if (object_type.kind == TypeKind::kArray) {
      if (object_type.element_type == model_.find_type("byte")) {
        if (const std::optional<IntegerMetaOperation> operation =
                integer_read_operation(meta.meta)) {
          ExpressionState state{model_.error_type(), ValueCategory::kCallable};
          state.integer_meta_operation = operation;
          return state;
        }
      }
      if (meta.meta == "length") {
        return ExpressionState{*model_.find_type("int32"),
                               ValueCategory::kValue};
      }
      diagnostics_.error(range, "array type '" + object_type.name +
                                    "' has no meta query '" +
                                    std::string{meta.meta} + "'");
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kString) {
      if (meta.meta == "length" || meta.meta == "byteLength") {
        return ExpressionState{*model_.find_type("int32"),
                               ValueCategory::kValue};
      }
      if (meta.meta == "isEmpty") {
        return ExpressionState{model_.bool_type(), ValueCategory::kValue};
      }
      diagnostics_.error(
          range, "string has no meta query '" + std::string{meta.meta} + "'");
      return ExpressionState{model_.error_type()};
    }

    diagnostics_.error(
        range, "type '" + object_type.name + "' has no Cloth meta queries");
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_safe_member_access(
      const SafeMemberAccessExpression& member, SourceRange range) {
    const ExpressionState object = analyze_expression(member.object);
    check_value(object, expression_range(member.object));
    if (object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (object.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    const SemanticType& nullable = model_.type(object.type);
    if (nullable.kind != TypeKind::kNullable || !nullable.element_type) {
      diagnostics_.error(range,
                         "safe member access requires a nullable "
                         "reference; found '" +
                             nullable.name + "'");
      return ExpressionState{model_.error_type()};
    }

    const SemanticType& object_type = model_.type(*nullable.element_type);
    if (object_type.kind == TypeKind::kArray) {
      if (member.member == "Length") {
        diagnostics_.error(range,
                           "safe meta queries are not supported; narrow the "
                           "array and use '::length'");
      } else {
        diagnostics_.error(range, "array type '" + object_type.name +
                                      "' has no member '" +
                                      std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kString) {
      if (member.member == "Length") {
        diagnostics_.error(range,
                           "safe meta queries are not supported; narrow the "
                           "string and use '::length'");
      } else if (member.member == "ByteLength") {
        diagnostics_.error(range,
                           "safe meta queries are not supported; narrow the "
                           "string and use '::byteLength'");
      } else if (member.member == "IsEmpty") {
        diagnostics_.error(range,
                           "safe meta queries are not supported; narrow the "
                           "string and use '::isEmpty'");
      } else {
        diagnostics_.error(
            range, "string has no member '" + std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kErrorClass && !object_type.file &&
        member.member == "Message") {
      return ExpressionState{model_.get_nullable_type(model_.string_type()),
                             ValueCategory::kValue,
                             error_message_symbol_,
                             {}};
    }
    if ((object_type.kind != TypeKind::kFileClass &&
         object_type.kind != TypeKind::kErrorClass &&
         object_type.kind != TypeKind::kInterface) ||
        !object_type.file) {
      diagnostics_.error(
          range, "type '" + object_type.name + "' has no Cloth members");
      return ExpressionState{model_.error_type()};
    }

    const FileId target_file = *object_type.file;
    std::vector<SymbolId> members = find_members(target_file, member.member);
    if (members.empty()) {
      if (has_inaccessible_member(target_file, member.member)) {
        diagnostics_.error(range, "member '" + std::string{member.member} +
                                      "' is private in file class '" +
                                      object_type.name + "'");
      } else {
        diagnostics_.error(range, "file class '" + object_type.name +
                                      "' has no member '" +
                                      std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }

    const SemanticSymbol& selected = model_.symbol(members.front());
    if (selected.kind != SymbolKind::kField) {
      diagnostics_.error(
          range,
          "safe function calls are not implemented; narrow the receiver "
          "first");
      return ExpressionState{model_.error_type()};
    }
    if (selected.is_static) {
      diagnostics_.error(range,
                         "static field '" + selected.name +
                             "' must be accessed through its file class");
      return ExpressionState{model_.error_type()};
    }
    if (!is_reference(selected.type)) {
      diagnostics_.error(range, "safe access to value-type field '" +
                                    selected.name +
                                    "' requires nullable value types");
      return ExpressionState{model_.error_type()};
    }

    const TypeId result = model_.type(selected.type).kind == TypeKind::kNullable
                              ? selected.type
                              : model_.get_nullable_type(selected.type);
    return ExpressionState{result, ValueCategory::kValue, members.front(), {}};
  }

  ExpressionState analyze_null_coalesce(const NullCoalesceExpression& coalesce,
                                        SourceRange range) {
    const ExpressionState nullable = analyze_expression(coalesce.nullable);
    check_value(nullable, expression_range(coalesce.nullable));
    const NonNullSet fallback_base = active_non_null_;
    const ExpressionState fallback =
        analyze_expression_with_effect_reachability(
            coalesce.fallback, nullable.type != model_.bottom_type());
    check_value(fallback, expression_range(coalesce.fallback));
    active_non_null_ = intersect_symbols(fallback_base, active_non_null_);

    if (nullable.type == model_.error_type() ||
        fallback.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (nullable.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    const SemanticType& nullable_type = model_.type(nullable.type);
    if (nullable_type.kind != TypeKind::kNullable ||
        !nullable_type.element_type) {
      diagnostics_.error(
          range,
          "left operand of the null-coalescing operator must be nullable; "
          "found '" +
              nullable_type.name + "'");
      return ExpressionState{model_.error_type()};
    }
    if (is_assignable(*nullable_type.element_type, fallback.type)) {
      return ExpressionState{*nullable_type.element_type,
                             ValueCategory::kValue};
    }
    if (is_assignable(nullable.type, fallback.type)) {
      return ExpressionState{nullable.type, ValueCategory::kValue};
    }
    diagnostics_.error(
        range, "right operand of the null-coalescing operator has type '" +
                   type_name(fallback.type) + "'; expected '" +
                   type_name(*nullable_type.element_type) + "' or '" +
                   nullable_type.name + "'");
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_null_assert(const NullAssertExpression& assertion,
                                      SourceRange range) {
    const ExpressionState operand = analyze_expression(assertion.operand);
    check_value(operand, expression_range(assertion.operand));
    if (operand.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (operand.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }
    const SemanticType& nullable = model_.type(operand.type);
    if (nullable.kind != TypeKind::kNullable || !nullable.element_type) {
      diagnostics_.error(range,
                         "non-null assertion requires a nullable reference; "
                         "found '" +
                             nullable.name + "'");
      return ExpressionState{model_.error_type()};
    }
    if (const std::optional<SymbolId> symbol =
            narrowable_symbol(assertion.operand)) {
      add_symbol(active_non_null_, *symbol);
    }
    return ExpressionState{*nullable.element_type, ValueCategory::kValue};
  }

  ExpressionState analyze_call(const CallExpression& call, SourceRange range) {
    const ExpressionState callee = analyze_expression(call.callee);
    std::vector<ExpressionState> arguments;
    arguments.reserve(call.arguments.size());
    bool has_error_argument = false;
    bool has_bottom_argument = false;
    for (const ExpressionId argument : call.arguments) {
      const bool argument_reachable =
          callee.type != model_.bottom_type() && !has_bottom_argument;
      const bool previous = effects_reachable_;
      effects_reachable_ = previous && argument_reachable;
      ExpressionState value = analyze_overload_argument(argument);
      effects_reachable_ = previous;
      check_value(value, expression_range(argument));
      has_error_argument =
          has_error_argument || value.type == model_.error_type();
      has_bottom_argument =
          has_bottom_argument || value.type == model_.bottom_type();
      arguments.push_back(std::move(value));
    }

    if (callee.type == model_.bottom_type()) {
      return ExpressionState{model_.bottom_type(), ValueCategory::kValue};
    }

    if (callee.integer_meta_operation) {
      return analyze_integer_meta_call(*callee.integer_meta_operation, call,
                                       arguments, range);
    }

    std::vector<SymbolId> candidates = callee.candidates;
    if (callee.category == ValueCategory::kType) {
      const SemanticType& type = model_.type(callee.type);
      if ((type.kind == TypeKind::kFileClass ||
           type.kind == TypeKind::kErrorClass ||
           type.kind == TypeKind::kStruct) &&
          type.file) {
        const FileSemantics& target = model_.file(*type.file);
        if (target.is_abstract) {
          diagnostics_.error(range, "abstract file class '" + type.name +
                                        "' cannot be constructed");
          return ExpressionState{model_.error_type()};
        }
        candidates = target.constructors;
      } else if (type.kind == TypeKind::kErrorClass &&
                 callee.type == model_.error_root_type()) {
        diagnostics_.error(range,
                           "abstract compiler error 'Error' cannot be "
                           "constructed");
        return ExpressionState{model_.error_type()};
      } else if (type.kind == TypeKind::kErrorClass &&
                 callee.type == model_.division_by_zero_type()) {
        candidates = {division_by_zero_constructor_};
      } else if (type.kind == TypeKind::kInterface) {
        diagnostics_.error(
            range, "interface '" + type.name + "' cannot be constructed");
        return ExpressionState{model_.error_type()};
      }
    } else if (callee.category != ValueCategory::kCallable) {
      if (callee.type != model_.error_type()) {
        diagnostics_.error(range, "expression is not callable");
      }
      return ExpressionState{model_.error_type()};
    }

    std::vector<SymbolId> matches;
    std::vector<SymbolId> exact_matches;
    std::vector<SymbolId> recovery_matches;
    for (const SymbolId candidate : candidates) {
      const SemanticSymbol& symbol = model_.symbol(candidate);
      if (symbol.parameter_types.size() != arguments.size()) {
        continue;
      }
      bool matches_types = true;
      bool matches_exactly = true;
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        bool argument_is_exact = false;
        if (!overload_argument_matches(call.arguments[index], arguments[index],
                                       symbol.parameter_types[index],
                                       argument_is_exact)) {
          matches_types = false;
          break;
        }
        matches_exactly = matches_exactly && argument_is_exact;
      }
      if (matches_types) {
        if (symbol.is_valid) {
          matches.push_back(candidate);
          if (matches_exactly) {
            exact_matches.push_back(candidate);
          }
        } else {
          recovery_matches.push_back(candidate);
        }
      }
    }

    if (!exact_matches.empty()) {
      matches = std::move(exact_matches);
    }

    if (matches.empty() && !recovery_matches.empty()) {
      matches.push_back(recovery_matches.front());
      has_error_argument = true;
    }

    if (has_error_argument && matches.empty()) {
      for (const SymbolId candidate : candidates) {
        if (model_.symbol(candidate).parameter_types.size() ==
            arguments.size()) {
          matches.push_back(candidate);
          break;
        }
      }
    }

    if (matches.empty()) {
      for (const ExpressionId argument : call.arguments) {
        if (is_numeric_literal_expression(argument) &&
            !numeric_literal_expression_constant(argument)) {
          invalidate_numeric_literal_expression(argument);
        }
      }
      diagnostics_.error(range, "no matching overload for call with " +
                                    std::to_string(arguments.size()) +
                                    " argument(s)");
      return ExpressionState{model_.error_type()};
    }
    if (matches.size() > 1 && !has_error_argument) {
      diagnostics_.error(range, "call is ambiguous between " +
                                    std::to_string(matches.size()) +
                                    " overloads");
      return ExpressionState{model_.error_type()};
    }

    const SymbolId selected = matches.front();
    ExpressionSemantics& callee_semantics =
        model_.mutable_file(current_file_).expressions.at(call.callee.value);
    callee_semantics.symbol = selected;
    const SemanticSymbol& symbol = model_.symbol(selected);
    if (!validate_call_access(call.callee, symbol, range)) {
      return ExpressionState{model_.error_type()};
    }
    apply_overload_argument_context(call.arguments, arguments,
                                    symbol.parameter_types);
    if (has_bottom_argument) {
      return ExpressionState{
          model_.bottom_type(), ValueCategory::kValue, selected, {}};
    }
    record_call_effect(selected, range);
    ExpressionState result{symbol.type, ValueCategory::kValue, selected, {}};
    result.interface_dispatch = callee.interface_dispatch;
    return result;
  }

  ExpressionState analyze_integer_meta_call(
      IntegerMetaOperation operation, const CallExpression& call,
      const std::vector<ExpressionState>& arguments, SourceRange range) {
    if (std::ranges::any_of(arguments, [this](const ExpressionState& argument) {
          return argument.type == model_.error_type();
        })) {
      return ExpressionState{model_.error_type()};
    }
    const TypeId int32_type = *model_.find_type("int32");
    if (operation.kind == IntegerMetaOperationKind::kWrite) {
      if (arguments.size() != 2) {
        diagnostics_.error(
            range, "integer endian write requires a byte[] and an offset");
        return ExpressionState{model_.error_type()};
      }
      const SemanticType& destination = model_.type(arguments[0].type);
      const bool is_byte_array =
          destination.kind == TypeKind::kArray &&
          destination.element_type == model_.find_type("byte");
      if (!is_byte_array) {
        diagnostics_.error(expression_range(call.arguments[0]),
                           "integer endian destination must be 'byte[]'");
      }
      if (!is_assignable(int32_type, arguments[1].type)) {
        diagnostics_.error(expression_range(call.arguments[1]),
                           "integer endian offset must be assignable to "
                           "'int32'");
      }
      if (!is_byte_array || !is_assignable(int32_type, arguments[1].type)) {
        return ExpressionState{model_.error_type()};
      }
      ExpressionState state{model_.void_type(), ValueCategory::kValue};
      state.integer_meta_operation = operation;
      return state;
    }

    if (arguments.size() != 1) {
      diagnostics_.error(range,
                         "integer endian read requires exactly one offset");
      return ExpressionState{model_.error_type()};
    }
    if (!is_assignable(int32_type, arguments[0].type)) {
      diagnostics_.error(expression_range(call.arguments[0]),
                         "integer endian offset must be assignable to "
                         "'int32'");
      return ExpressionState{model_.error_type()};
    }
    ExpressionState state{operation.integer_type, ValueCategory::kValue};
    state.integer_meta_operation = operation;
    return state;
  }

  std::optional<IntegerMetaOperation> integer_read_operation(
      std::string_view name) const {
    IntegerByteOrder byte_order = IntegerByteOrder::kLittleEndian;
    constexpr std::string_view kLittleEndian = "LittleEndian";
    constexpr std::string_view kBigEndian = "BigEndian";
    if (name.ends_with(kLittleEndian)) {
      name.remove_suffix(kLittleEndian.size());
    } else if (name.ends_with(kBigEndian)) {
      name.remove_suffix(kBigEndian.size());
      byte_order = IntegerByteOrder::kBigEndian;
    } else {
      return std::nullopt;
    }

    std::string_view type_name;
    if (name == "readByte") {
      type_name = "byte";
    } else if (name == "readInt8") {
      type_name = "int8";
    } else if (name == "readInt16") {
      type_name = "int16";
    } else if (name == "readInt32") {
      type_name = "int32";
    } else if (name == "readInt64") {
      type_name = "int64";
    } else if (name == "readUint8") {
      type_name = "uint8";
    } else if (name == "readUint16") {
      type_name = "uint16";
    } else if (name == "readUint32") {
      type_name = "uint32";
    } else if (name == "readUint64") {
      type_name = "uint64";
    } else {
      return std::nullopt;
    }
    return IntegerMetaOperation{IntegerMetaOperationKind::kRead, byte_order,
                                *model_.find_type(type_name)};
  }

  bool validate_call_access(ExpressionId callee_id,
                            const SemanticSymbol& callable, SourceRange range) {
    if (callable.kind == SymbolKind::kConstructor) {
      if (callable.visibility == Visibility::kPublic ||
          callable.file == current_file_) {
        return true;
      }
      const std::string owner_name =
          callable.file ? model_.symbol(model_.file(*callable.file).symbol).name
                        : std::string{"<unknown>"};
      diagnostics_.error(range,
                         "constructor for '" + owner_name + "' is private");
      diagnostics_.note(callable.range, "private constructor is declared here");
      return false;
    }
    if (callable.kind != SymbolKind::kFunction ||
        callable.intrinsic != IntrinsicKind::kNone) {
      return true;
    }
    const Expression& callee =
        files_[current_file_.value]->storage.expression(callee_id);
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&callee.data)) {
      const bool is_valid =
          validate_call_access(grouped->expression, callable, range);
      if (model_.file(current_file_)
              .expressions.at(grouped->expression.value)
              .is_base_qualified) {
        model_.mutable_file(current_file_)
            .expressions.at(callee_id.value)
            .is_base_qualified = true;
      }
      return is_valid;
    }
    const auto* member = std::get_if<MemberAccessExpression>(&callee.data);
    if (member == nullptr) {
      if (analyzing_base_initializer_ && !callable.is_static) {
        diagnostics_.error(range, "cannot call instance function '" +
                                      callable.name +
                                      "' in a base constructor initializer");
        return false;
      }
      if (!callable.is_static && !has_implicit_receiver_) {
        diagnostics_.error(range, "instance function '" + callable.name +
                                      "' is unavailable in a static context");
        return false;
      }
      return true;
    }

    const ExpressionSemantics& object =
        model_.file(current_file_).expressions.at(member->object.value);
    if (object.category == ValueCategory::kSuper) {
      if (callable.is_abstract) {
        diagnostics_.error(range, "'super' cannot call abstract function '" +
                                      callable.name + "'");
        return false;
      }
      if (callable.is_static) {
        diagnostics_.error(range, "'super' cannot qualify static function '" +
                                      callable.name + "'");
        return false;
      }
      model_.mutable_file(current_file_)
          .expressions.at(callee_id.value)
          .is_base_qualified = true;
      return true;
    }
    if (callable.is_static && object.category != ValueCategory::kType) {
      diagnostics_.error(range,
                         "static function '" + callable.name +
                             "' must be accessed through its file class");
      return false;
    }
    if (!callable.is_static && object.category == ValueCategory::kType) {
      const SemanticType& qualifier = model_.type(object.type);
      if (qualifier.file && *qualifier.file != current_file_ &&
          is_file_class_subtype(model_.file(current_file_).type, object.type)) {
        diagnostics_.error(range,
                           "base instance calls must use 'super.Method(...)'");
        return false;
      }
      diagnostics_.error(
          range, "function '" + callable.name + "' requires an instance");
      return false;
    }
    return true;
  }

  bool is_static_scalar_type(TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return kind == TypeKind::kBool || kind == TypeKind::kChar ||
           kind == TypeKind::kByte || kind == TypeKind::kInt8 ||
           kind == TypeKind::kInt16 || kind == TypeKind::kInt32 ||
           kind == TypeKind::kInt64 || kind == TypeKind::kUint8 ||
           kind == TypeKind::kUint16 || kind == TypeKind::kUint32 ||
           kind == TypeKind::kUint64 || kind == TypeKind::kFloat32 ||
           kind == TypeKind::kFloat64 || kind == TypeKind::kEnum;
  }

  ExpressionState record_expression(ExpressionId id, ExpressionState state) {
    model_.mutable_file(current_file_).expressions.at(id.value) =
        ExpressionSemantics{state.type,
                            state.category,
                            state.symbol,
                            false,
                            state.checked_type,
                            false,
                            state.interface_dispatch,
                            state.integer_meta_operation,
                            state.may_divide_by_zero};
    return state;
  }

  void index_members() {
    member_names_.resize(model_.files().size());
    for (std::size_t i = 0; i < model_.files().size(); ++i) {
      const auto& file = model_.file(FileId{i});
      for (const auto symbol : file.fields)
        member_names_[i][model_.symbol(symbol).name].push_back(symbol);
      for (const auto symbol : file.functions)
        member_names_[i][model_.symbol(symbol).name].push_back(symbol);
    }
  }

  std::vector<SymbolId> declared_members(FileId file_id,
                                         std::string_view name) const {
    const FileSemantics& file = model_.file(file_id);
    std::vector<SymbolId> matches;
    if (file.kind == FileTypeKind::kInterface) {
      for (const SymbolId symbol_id : file.interface_functions) {
        if (model_.symbol(symbol_id).name == name) {
          matches.push_back(symbol_id);
        }
      }
      return matches;
    }
    const auto found = member_names_[file_id.value].find(name);
    return found == member_names_[file_id.value].end() ? std::vector<SymbolId>{}
                                                       : found->second;
  }

  std::vector<SymbolId> find_members(FileId file_id,
                                     std::string_view name) const {
    if (model_.file(file_id).kind == FileTypeKind::kInterface) {
      return declared_members(file_id, name);
    }
    std::optional<FileId> owner = file_id;
    for (std::size_t depth = 0; owner && depth < model_.files().size();
         ++depth) {
      std::vector<SymbolId> matches = declared_members(*owner, name);
      if (!matches.empty()) {
        std::erase_if(matches, [this, owner](SymbolId symbol_id) {
          const SemanticSymbol& symbol = model_.symbol(symbol_id);
          return *owner != current_file_ &&
                 symbol.visibility == Visibility::kPrivate;
        });
        return matches;
      }
      owner = model_.file(*owner).base_file;
    }
    if (model_.file(file_id).kind == FileTypeKind::kError &&
        name == "Message") {
      return {error_message_symbol_};
    }
    return {};
  }

  bool has_inaccessible_member(FileId file_id, std::string_view name) const {
    if (model_.file(file_id).kind == FileTypeKind::kInterface) {
      return false;
    }
    std::optional<FileId> owner = file_id;
    for (std::size_t depth = 0; owner && depth < model_.files().size();
         ++depth) {
      const std::vector<SymbolId> matches = declared_members(*owner, name);
      if (!matches.empty()) {
        return *owner != current_file_ &&
               std::ranges::any_of(matches, [this](SymbolId symbol_id) {
                 return model_.symbol(symbol_id).visibility ==
                        Visibility::kPrivate;
               });
      }
      owner = model_.file(*owner).base_file;
    }
    return false;
  }

  std::optional<FileId> find_qualified_file(std::string_view name) const {
    for (std::size_t index = 0; index < model_.files().size(); ++index) {
      const FileSemantics& file = model_.file(FileId{index});
      if (model_.symbol(file.symbol).name == name &&
          file.type != model_.error_type()) {
        return FileId{index};
      }
    }
    return std::nullopt;
  }

  std::optional<FileId> find_visible_file(FileId current_file,
                                          std::string_view name) const {
    for (const VisibleFile& binding : visible_files_[current_file.value]) {
      if (binding.name == name &&
          model_.file(binding.file).type != model_.error_type()) {
        return binding.file;
      }
    }
    const auto prelude = prelude_files_.find(name);
    if (prelude != prelude_files_.end() &&
        model_.file(prelude->second).type != model_.error_type()) {
      return prelude->second;
    }
    return std::nullopt;
  }

  std::optional<FileId> find_inaccessible_file(FileId current_file,
                                               std::string_view name) const {
    const std::string& package_name = files_[current_file.value]->package_name;
    const std::string& owning_package =
        files_[current_file.value]->owning_package;
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (index == current_file.value || files_[index]->name != name ||
          files_[index]->package_name != package_name ||
          files_[index]->owning_package != owning_package) {
        continue;
      }
      const FileId file{index};
      const SemanticSymbol& symbol = model_.symbol(model_.file(file).symbol);
      if (symbol.visibility == Visibility::kPrivate) {
        return file;
      }
    }
    return std::nullopt;
  }

  bool mark_presence_test(const ExpressionState& state, ExpressionId id) {
    if (state.type == model_.error_type() ||
        model_.type(state.type).kind != TypeKind::kNullable) {
      return false;
    }
    model_.mutable_file(current_file_)
        .expressions.at(id.value)
        .is_presence_test = true;
    return true;
  }

  void check_condition(const ExpressionState& state, ExpressionId id,
                       std::string_view context) {
    const SourceRange range = expression_range(id);
    if (!check_value(state, range) || state.type == model_.error_type() ||
        state.type == model_.bottom_type() ||
        state.type == model_.bool_type() || mark_presence_test(state, id)) {
      return;
    }
    if (is_reference(state.type)) {
      diagnostics_.error(range, std::string{context} +
                                    " uses a non-null reference and is always "
                                    "true");
      return;
    }
    check_assignment(model_.bool_type(), state.type, range, context);
  }

  bool check_value(const ExpressionState& state, SourceRange range) {
    if (state.type == model_.bottom_type()) {
      return true;
    }
    if (state.category == ValueCategory::kCallable) {
      diagnostics_.error(range, "function reference must be called");
      return false;
    }
    if (state.category == ValueCategory::kType) {
      diagnostics_.error(range, "type reference cannot be used as a value");
      return false;
    }
    if (state.category == ValueCategory::kSuper) {
      diagnostics_.error(range,
                         "'super' must qualify an instance function call");
      return false;
    }
    if (state.type == model_.void_type()) {
      diagnostics_.error(range, "void expression cannot be used as a value");
      return false;
    }
    return state.category != ValueCategory::kInvalid ||
           state.type == model_.error_type();
  }

  void check_assignment(TypeId expected, TypeId actual, SourceRange range,
                        std::string_view context) {
    if (!is_assignable(expected, actual)) {
      diagnostics_.error(range, std::string{context} + " has type '" +
                                    type_name(actual) + "'; expected '" +
                                    type_name(expected) + "'");
    }
  }

  bool is_assignable(TypeId expected, TypeId actual) const {
    if (expected == model_.error_type() || actual == model_.error_type()) {
      return true;
    }
    if (actual == model_.bottom_type()) {
      return true;
    }
    if (expected == model_.void_type() || actual == model_.void_type()) {
      return false;
    }
    if (expected == actual) {
      return true;
    }
    const SemanticType& expected_type = model_.type(expected);
    const SemanticType& actual_type = model_.type(actual);
    if (can_widen_numeric(actual_type.kind, expected_type.kind)) {
      return true;
    }
    if ((expected_type.kind == TypeKind::kFileClass ||
         expected_type.kind == TypeKind::kErrorClass) &&
        (actual_type.kind == TypeKind::kFileClass ||
         actual_type.kind == TypeKind::kErrorClass) &&
        is_file_class_subtype(actual, expected)) {
      return true;
    }
    if (expected_type.kind == TypeKind::kInterface &&
        (actual_type.kind == TypeKind::kFileClass ||
         actual_type.kind == TypeKind::kErrorClass ||
         actual_type.kind == TypeKind::kInterface) &&
        implements_interface(actual, expected)) {
      return true;
    }
    if (expected_type.kind == TypeKind::kObject) {
      return actual_type.kind == TypeKind::kString ||
             actual_type.kind == TypeKind::kFileClass ||
             actual_type.kind == TypeKind::kErrorClass ||
             actual_type.kind == TypeKind::kInterface ||
             actual_type.kind == TypeKind::kArray;
    }
    if (expected_type.kind != TypeKind::kNullable ||
        !expected_type.element_type) {
      return false;
    }
    if (actual == model_.null_type() ||
        is_assignable(*expected_type.element_type, actual)) {
      return true;
    }
    return actual_type.kind == TypeKind::kNullable &&
           actual_type.element_type &&
           is_assignable(*expected_type.element_type,
                         *actual_type.element_type);
  }

  bool is_override_return_compatible(TypeId inherited,
                                     TypeId overriding) const {
    if (inherited == overriding) {
      return true;
    }
    return is_reference(inherited) && is_reference(overriding) &&
           is_assignable(inherited, overriding);
  }

  bool is_file_class_subtype(TypeId subtype, TypeId supertype) const {
    if (subtype == supertype) {
      return true;
    }
    const SemanticType& subtype_info = model_.type(subtype);
    const SemanticType& supertype_info = model_.type(supertype);
    if (subtype_info.kind == TypeKind::kErrorClass &&
        supertype == model_.error_root_type()) {
      return true;
    }
    if ((subtype_info.kind != TypeKind::kFileClass &&
         subtype_info.kind != TypeKind::kErrorClass) ||
        !subtype_info.file ||
        (supertype_info.kind != TypeKind::kFileClass &&
         supertype_info.kind != TypeKind::kErrorClass) ||
        !supertype_info.file) {
      return false;
    }
    std::optional<FileId> current = model_.file(*subtype_info.file).base_file;
    for (std::size_t depth = 0; current && depth < model_.files().size();
         ++depth) {
      if (*current == *supertype_info.file) {
        return true;
      }
      current = model_.file(*current).base_file;
    }
    return false;
  }

  bool implements_interface(TypeId type, TypeId interface_type) const {
    const SemanticType& type_info = model_.type(type);
    const SemanticType& interface_info = model_.type(interface_type);
    if ((type_info.kind != TypeKind::kFileClass &&
         type_info.kind != TypeKind::kErrorClass &&
         type_info.kind != TypeKind::kInterface) ||
        !type_info.file || interface_info.kind != TypeKind::kInterface ||
        !interface_info.file) {
      return false;
    }
    const std::vector<FileId>& interfaces =
        model_.file(*type_info.file).interfaces;
    return std::ranges::find(interfaces, *interface_info.file) !=
           interfaces.end();
  }

  bool is_reference(TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return kind == TypeKind::kString || kind == TypeKind::kObject ||
           kind == TypeKind::kFileClass || kind == TypeKind::kErrorClass ||
           kind == TypeKind::kInterface || kind == TypeKind::kArray ||
           kind == TypeKind::kNullable;
  }

  bool is_error_type(TypeId type) const {
    return type.value < model_.types().size() &&
           model_.type(type).kind == TypeKind::kErrorClass;
  }

  bool is_error_subtype(TypeId subtype, TypeId supertype) const {
    return is_error_type(subtype) && is_error_type(supertype) &&
           is_file_class_subtype(subtype, supertype);
  }

  bool integer_literal_fits(std::string_view lexeme, TypeId type,
                            bool is_negated) const {
    const std::optional<NumericTypeProperties> properties =
        numeric_type_properties(model_.type(type).kind);
    if (!properties ||
        properties->category == NumericCategory::kFloatingPoint) {
      return false;
    }
    if (is_negated &&
        properties->category == NumericCategory::kUnsignedInteger) {
      return false;
    }

    std::uint64_t value = 0;
    const char* const begin = lexeme.data();
    const char* const end = begin + lexeme.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
      return false;
    }

    std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (properties->category == NumericCategory::kSignedInteger) {
      maximum = std::uint64_t{1} << (properties->bit_width - 1);
      if (!is_negated) {
        --maximum;
      }
    } else if (properties->bit_width < 64) {
      maximum = (std::uint64_t{1} << properties->bit_width) - 1;
    }
    return value <= maximum;
  }

  std::optional<ShiftLiteral> shift_literal(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* literal =
            std::get_if<LiteralExpression>(&expression.data)) {
      if (literal->kind != LiteralKind::kInteger) {
        return std::nullopt;
      }
      const NumericLiteralSpelling spelling =
          parse_numeric_literal_spelling(literal->lexeme);
      if (spelling.error != NumericLiteralSpellingError::kNone) {
        return std::nullopt;
      }
      std::uint64_t value = 0;
      const char* const begin = spelling.core.data();
      const char* const end = begin + spelling.core.size();
      const auto parsed = std::from_chars(begin, end, value);
      if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
      }
      return ShiftLiteral{false, value};
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data)) {
      if (unary->operation != TokenKind::kPlus &&
          unary->operation != TokenKind::kMinus) {
        return std::nullopt;
      }
      std::optional<ShiftLiteral> value = shift_literal(unary->operand);
      if (value && unary->operation == TokenKind::kMinus &&
          value->magnitude != 0) {
        value->is_negative = !value->is_negative;
      }
      return value;
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return shift_literal(grouped->expression);
    }
    return std::nullopt;
  }

  bool check_shift_literal(ExpressionId count, TypeId value_type) {
    const std::optional<ShiftLiteral> literal = shift_literal(count);
    const std::optional<NumericTypeProperties> properties =
        numeric_type_properties(model_.type(value_type).kind);
    if (!literal || !properties) {
      return true;
    }
    if (!literal->is_negative && literal->magnitude < properties->bit_width) {
      return true;
    }
    diagnostics_.error(
        expression_range(count),
        "shift count is out of range for '" + type_name(value_type) + "'");
    return false;
  }

  bool floating_literal_fits(std::string_view lexeme, TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return scalar_literal(LiteralKind::kFloat, lexeme, kind).has_value();
  }

  std::optional<TypeId> common_numeric_type(TypeId left, TypeId right) const {
    if (left == model_.error_type() || right == model_.error_type()) {
      return std::nullopt;
    }
    const TypeKind left_kind = model_.type(left).kind;
    const TypeKind right_kind = model_.type(right).kind;
    if (!is_numeric_type(left_kind) || !is_numeric_type(right_kind)) {
      return std::nullopt;
    }
    if (left == right) {
      return left;
    }
    if (can_widen_numeric(left_kind, right_kind)) {
      return right;
    }
    if (can_widen_numeric(right_kind, left_kind)) {
      return left;
    }
    return std::nullopt;
  }

  std::optional<TypeId> common_binary_numeric_type(
      const BinaryExpression& binary, ExpressionState& left,
      ExpressionState& right) {
    if (const std::optional<TypeId> common =
            common_numeric_type(left.type, right.type)) {
      contextualize_numeric_expression(binary.left, *common);
      contextualize_numeric_expression(binary.right, *common);
      return common;
    }
    if (is_numeric(right.type) &&
        contextualize_numeric_expression(binary.left, right.type)) {
      left.type = right.type;
      return right.type;
    }
    if (is_numeric(left.type) &&
        contextualize_numeric_expression(binary.right, left.type)) {
      right.type = left.type;
      return left.type;
    }
    return std::nullopt;
  }

  bool is_numeric_literal_expression(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* literal =
            std::get_if<LiteralExpression>(&expression.data)) {
      return literal->kind == LiteralKind::kInteger ||
             literal->kind == LiteralKind::kFloat;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data)) {
      return (unary->operation == TokenKind::kPlus ||
              unary->operation == TokenKind::kMinus) &&
             is_numeric_literal_expression(unary->operand);
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return is_numeric_literal_expression(grouped->expression);
    }
    return false;
  }

  bool is_contextual_numeric_literal_expression(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* literal =
            std::get_if<LiteralExpression>(&expression.data)) {
      if (literal->kind != LiteralKind::kInteger &&
          literal->kind != LiteralKind::kFloat) {
        return false;
      }
      const NumericLiteralSpelling spelling =
          parse_numeric_literal_spelling(literal->lexeme);
      return spelling.error == NumericLiteralSpellingError::kNone &&
             spelling.suffix_kind == NumericLiteralSuffix::kNone;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data)) {
      return (unary->operation == TokenKind::kPlus ||
              unary->operation == TokenKind::kMinus) &&
             is_contextual_numeric_literal_expression(unary->operand);
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return is_contextual_numeric_literal_expression(grouped->expression);
    }
    return false;
  }

  std::optional<ScalarConstant> numeric_literal_expression_constant(
      ExpressionId id) const {
    std::vector<TokenKind> signs;
    for (;;) {
      const Expression& expression =
          files_[current_file_.value]->storage.expression(id);
      if (const auto* grouped =
              std::get_if<ParenthesizedExpression>(&expression.data)) {
        id = grouped->expression;
      } else if (const auto* unary =
                     std::get_if<UnaryExpression>(&expression.data)) {
        if (unary->operation != TokenKind::kPlus &&
            unary->operation != TokenKind::kMinus) {
          return std::nullopt;
        }
        signs.push_back(unary->operation);
        id = unary->operand;
      } else if (const auto* literal =
                     std::get_if<LiteralExpression>(&expression.data);
                 literal != nullptr &&
                 (literal->kind == LiteralKind::kInteger ||
                  literal->kind == LiteralKind::kFloat)) {
        const NumericLiteralSpelling spelling =
            parse_numeric_literal_spelling(literal->lexeme);
        if (spelling.error != NumericLiteralSpellingError::kNone) {
          return std::nullopt;
        }
        const TypeId type =
            model_.file(current_file_).expressions.at(id.value).type;
        if (type == model_.error_type()) {
          return std::nullopt;
        }
        const ConstantBits bits = scalar_signed_literal(
            literal->kind, spelling.core, model_.type(type).kind, signs);
        return bits ? std::optional{ScalarConstant{type, *bits}} : std::nullopt;
      } else {
        return std::nullopt;
      }
    }
  }

  bool integer_divisor_is_proven_nonzero(ExpressionId id) const {
    const std::optional<ScalarConstant> constant =
        numeric_literal_expression_constant(id);
    return constant && is_integer(constant->type) && constant->bits != 0;
  }

  void invalidate_numeric_literal_expression(ExpressionId id) {
    model_.mutable_file(current_file_).expressions.at(id.value).type =
        model_.error_type();
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data);
        unary != nullptr && (unary->operation == TokenKind::kPlus ||
                             unary->operation == TokenKind::kMinus)) {
      invalidate_numeric_literal_expression(unary->operand);
    } else if (const auto* grouped =
                   std::get_if<ParenthesizedExpression>(&expression.data)) {
      invalidate_numeric_literal_expression(grouped->expression);
    }
  }

  bool numeric_literal_expression_fits(ExpressionId id, TypeId target,
                                       bool is_negated = false) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* literal =
            std::get_if<LiteralExpression>(&expression.data)) {
      const NumericLiteralSpelling spelling =
          parse_numeric_literal_spelling(literal->lexeme);
      if (spelling.error != NumericLiteralSpellingError::kNone) {
        return false;
      }
      if (literal->kind == LiteralKind::kInteger && is_integer(target)) {
        return integer_literal_fits(spelling.core, target, is_negated);
      }
      return literal->kind == LiteralKind::kFloat &&
             is_floating_point(target) &&
             floating_literal_fits(spelling.core, target);
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data);
        unary != nullptr && (unary->operation == TokenKind::kPlus ||
                             unary->operation == TokenKind::kMinus)) {
      const bool child_is_negated =
          is_negated != (unary->operation == TokenKind::kMinus);
      return numeric_literal_expression_fits(unary->operand, target,
                                             child_is_negated);
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return numeric_literal_expression_fits(grouped->expression, target,
                                             is_negated);
    }
    return false;
  }

  bool numeric_literal_conversion_fits(ExpressionId id, TypeId target,
                                       bool is_negated = false) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* literal =
            std::get_if<LiteralExpression>(&expression.data)) {
      const NumericLiteralSpelling spelling =
          parse_numeric_literal_spelling(literal->lexeme);
      if (spelling.error != NumericLiteralSpellingError::kNone) {
        return false;
      }
      return scalar_literal(literal->kind, spelling.core,
                            model_.type(target).kind, is_negated)
          .has_value();
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data);
        unary != nullptr && (unary->operation == TokenKind::kPlus ||
                             unary->operation == TokenKind::kMinus)) {
      const bool child_is_negated =
          is_negated != (unary->operation == TokenKind::kMinus);
      return numeric_literal_conversion_fits(unary->operand, target,
                                             child_is_negated);
    }
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return numeric_literal_conversion_fits(grouped->expression, target,
                                             is_negated);
    }
    return false;
  }

  ExpressionState analyze_overload_argument(ExpressionId id) {
    const bool previous = defer_default_numeric_literal_range_;
    defer_default_numeric_literal_range_ =
        previous || is_contextual_numeric_literal_expression(id);
    ExpressionState state = analyze_expression(id);
    defer_default_numeric_literal_range_ = previous;
    return state;
  }

  bool overload_argument_matches(ExpressionId id,
                                 const ExpressionState& argument,
                                 TypeId parameter, bool& is_exact) const {
    is_exact = false;
    if (argument.type == model_.error_type() ||
        argument.type == model_.bottom_type() ||
        parameter == model_.error_type()) {
      return true;
    }
    if (is_contextual_numeric_literal_expression(id)) {
      if (!numeric_literal_expression_fits(id, parameter)) {
        return false;
      }
      is_exact = argument.type == parameter;
      return true;
    }
    is_exact = argument.type == parameter;
    return is_assignable(parameter, argument.type);
  }

  void apply_overload_argument_context(std::span<const ExpressionId> ids,
                                       std::span<ExpressionState> arguments,
                                       std::span<const TypeId> parameters) {
    const std::size_t count =
        std::min({ids.size(), arguments.size(), parameters.size()});
    for (std::size_t index = 0; index < count; ++index) {
      if (arguments[index].type != model_.error_type() &&
          contextualize_numeric_expression(ids[index], parameters[index])) {
        arguments[index].type = parameters[index];
      }
    }
  }

  bool contextualize_numeric_expression(ExpressionId id, TypeId target,
                                        bool is_negated = false) {
    if (!is_contextual_numeric_literal_expression(id)) {
      return false;
    }
    if (!numeric_literal_expression_fits(id, target, is_negated)) {
      return false;
    }
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (std::holds_alternative<LiteralExpression>(expression.data)) {
      model_.mutable_file(current_file_).expressions.at(id.value).type = target;
      return true;
    } else if (const auto* unary =
                   std::get_if<UnaryExpression>(&expression.data);
               unary != nullptr && (unary->operation == TokenKind::kPlus ||
                                    unary->operation == TokenKind::kMinus)) {
      const bool child_is_negated =
          is_negated != (unary->operation == TokenKind::kMinus);
      static_cast<void>(contextualize_numeric_expression(unary->operand, target,
                                                         child_is_negated));
    } else if (const auto* grouped =
                   std::get_if<ParenthesizedExpression>(&expression.data)) {
      static_cast<void>(contextualize_numeric_expression(grouped->expression,
                                                         target, is_negated));
    }
    model_.mutable_file(current_file_).expressions.at(id.value).type = target;
    return true;
  }

  bool contextualize_numeric_conversion(ExpressionId id, TypeId target,
                                        bool is_negated = false) {
    if (!numeric_literal_conversion_fits(id, target, is_negated)) {
      return false;
    }
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* unary = std::get_if<UnaryExpression>(&expression.data);
        unary != nullptr && (unary->operation == TokenKind::kPlus ||
                             unary->operation == TokenKind::kMinus)) {
      const bool child_is_negated =
          is_negated != (unary->operation == TokenKind::kMinus);
      static_cast<void>(contextualize_numeric_conversion(unary->operand, target,
                                                         child_is_negated));
    } else if (const auto* grouped =
                   std::get_if<ParenthesizedExpression>(&expression.data)) {
      static_cast<void>(contextualize_numeric_conversion(grouped->expression,
                                                         target, is_negated));
    }
    model_.mutable_file(current_file_).expressions.at(id.value).type = target;
    return true;
  }

  bool is_integer(TypeId type) const {
    return is_integer_type(model_.type(type).kind);
  }

  bool is_floating_point(TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return kind == TypeKind::kFloat32 || kind == TypeKind::kFloat64;
  }

  bool is_numeric(TypeId type) const {
    return is_numeric_type(model_.type(type).kind);
  }

  void report_operator_type(TokenKind operation, SourceRange range,
                            TypeId operand) {
    diagnostics_.error(
        range, "operator '" + std::string{token_kind_name(operation)} +
                   "' cannot be applied to '" + type_name(operand) + "'");
  }

  void record_direct_error(TypeId type, SourceRange range) {
    if (!flow_reachable_ || !effects_reachable_) {
      return;
    }
    const ErrorEffectSource source{type, std::nullopt, range};
    if (current_callable_symbol_) {
      callable_effects_[current_callable_symbol_->value].push_back(source);
    } else if (current_effect_field_) {
      field_effects_[current_effect_field_->value].push_back(source);
    }
  }

  void record_call_effect(SymbolId callee, SourceRange range) {
    if (!flow_reachable_ || !effects_reachable_) {
      return;
    }
    const ErrorEffectSource source{std::nullopt, callee, range};
    if (current_callable_symbol_) {
      callable_effects_[current_callable_symbol_->value].push_back(source);
    } else if (current_effect_field_) {
      field_effects_[current_effect_field_->value].push_back(source);
    }
  }

  bool throws_set_covers(std::span<const TypeId> allowed,
                         std::span<const TypeId> actual) const {
    return std::ranges::all_of(actual, [this, allowed](TypeId type) {
      return std::ranges::any_of(allowed, [this, type](TypeId permitted) {
        return is_error_subtype(type, permitted);
      });
    });
  }

  std::vector<TypeId> normalize_error_set(std::vector<TypeId> types) const {
    std::ranges::sort(types, [this](TypeId left, TypeId right) {
      return canonical_type_identity(left, model_) <
             canonical_type_identity(right, model_);
    });
    types.erase(std::unique(types.begin(), types.end()), types.end());
    std::vector<TypeId> result;
    for (const TypeId type : types) {
      const bool covered =
          std::ranges::any_of(types, [this, type](TypeId other) {
            return other != type && is_error_subtype(type, other);
          });
      if (!covered) {
        result.push_back(type);
      }
    }
    return result;
  }

  std::vector<TypeId> intersect_error_sets(
      std::span<const TypeId> left, std::span<const TypeId> right) const {
    std::vector<TypeId> result;
    for (const TypeId left_type : left) {
      for (const TypeId right_type : right) {
        if (is_error_subtype(left_type, right_type)) {
          result.push_back(left_type);
        } else if (is_error_subtype(right_type, left_type)) {
          result.push_back(right_type);
        }
      }
    }
    return normalize_error_set(std::move(result));
  }

  bool infers_throws(SymbolId symbol_id) const {
    const SemanticSymbol& symbol = model_.symbol(symbol_id);
    return symbol.file && symbol.visibility == Visibility::kPrivate &&
           !symbol.has_explicit_throws &&
           (symbol.kind == SymbolKind::kFunction ||
            symbol.kind == SymbolKind::kConstructor);
  }

  std::vector<TypeId> source_effects(
      std::span<const ErrorEffectSource> sources,
      const std::map<std::size_t, std::vector<TypeId>>& inferred) const {
    std::vector<TypeId> types;
    for (const ErrorEffectSource& source : sources) {
      if (source.direct_type) {
        types.push_back(*source.direct_type);
        continue;
      }
      if (!source.callee || source.callee->value >= model_.symbols().size()) {
        continue;
      }
      if (const auto found = inferred.find(source.callee->value);
          found != inferred.end()) {
        types.insert(types.end(), found->second.begin(), found->second.end());
      } else {
        const std::span<const TypeId> declared =
            model_.symbol(*source.callee).thrown_types.span();
        types.insert(types.end(), declared.begin(), declared.end());
      }
    }
    return normalize_error_set(std::move(types));
  }

  void analyze_error_effects() {
    std::vector<SymbolId> callables;
    for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
      const FileSemantics& file = model_.file(FileId{file_index});
      callables.insert(callables.end(), file.functions.begin(),
                       file.functions.end());
      callables.insert(callables.end(), file.constructors.begin(),
                       file.constructors.end());
      const auto fields = field_effects_.find(file_index);
      if (fields != field_effects_.end()) {
        for (const SymbolId constructor : file.constructors) {
          if (std::ranges::find(incomplete_base_initializers_, constructor) !=
              incomplete_base_initializers_.end()) {
            continue;
          }
          auto& effects = callable_effects_[constructor.value];
          effects.insert(effects.end(), fields->second.begin(),
                         fields->second.end());
        }
      }
    }

    std::map<std::size_t, std::vector<TypeId>> inferred;
    for (const SymbolId callable : callables) {
      if (infers_throws(callable)) {
        inferred.emplace(callable.value, std::vector<TypeId>{});
      }
    }
    for (std::size_t iteration = 0; iteration <= inferred.size(); ++iteration) {
      bool changed = false;
      for (auto& [symbol, types] : inferred) {
        const auto sources = callable_effects_.find(symbol);
        const std::vector<TypeId> next =
            sources == callable_effects_.end()
                ? std::vector<TypeId>{}
                : source_effects(sources->second, inferred);
        if (next != types) {
          types = next;
          changed = true;
        }
      }
      if (!changed) {
        break;
      }
    }
    for (const auto& [symbol, types] : inferred) {
      model_.mutable_symbol(SymbolId{symbol}).thrown_types = types;
    }

    for (const SymbolId callable : callables) {
      if (infers_throws(callable)) {
        continue;
      }
      const auto found = callable_effects_.find(callable.value);
      if (found == callable_effects_.end()) {
        continue;
      }
      const SemanticSymbol& symbol = model_.symbol(callable);
      std::vector<std::pair<TypeId, SourceRange>> uncovered;
      for (const ErrorEffectSource& source : found->second) {
        std::vector<TypeId> types;
        if (source.direct_type) {
          types.push_back(*source.direct_type);
        } else if (source.callee) {
          if (const auto inferred_callee = inferred.find(source.callee->value);
              inferred_callee != inferred.end()) {
            types = inferred_callee->second;
          } else if (source.callee->value < model_.symbols().size()) {
            const ErrorTypeSet& declared =
                model_.symbol(*source.callee).thrown_types;
            types.assign(declared.begin(), declared.end());
          }
        }
        for (const TypeId type : types) {
          if (!throws_set_covers(symbol.thrown_types.span(),
                                 std::span<const TypeId>{&type, 1}) &&
              std::ranges::find(uncovered, std::pair{type, source.range}) ==
                  uncovered.end()) {
            uncovered.emplace_back(type, source.range);
          }
        }
      }
      for (const auto& [type, range] : uncovered) {
        diagnostics_.error(range,
                           std::string{symbol_kind_name(symbol.kind)} + " '" +
                               callable_signature(symbol) + "' may throw '" +
                               type_name(type) +
                               "', which is not covered by its throws clause");
        model_.mutable_symbol(callable).is_valid = false;
        if (symbol.file) {
          model_.mutable_file(*symbol.file).is_valid = false;
        }
      }
    }
  }

  std::string callable_signature(const SemanticSymbol& symbol) const {
    std::string signature = symbol.name + '(';
    for (std::size_t index = 0; index < symbol.parameter_types.size();
         ++index) {
      if (index != 0) {
        signature += ", ";
      }
      signature += type_name(symbol.parameter_types[index]);
    }
    signature += "): ";
    signature += type_name(symbol.type);
    return signature;
  }

  std::string type_name(TypeId type) const { return model_.type(type).name; }

  SourceRange expression_range(ExpressionId id) const {
    return files_[current_file_.value]->storage.expression(id).range;
  }

  std::span<const FileClassDecl* const> files_;
  DiagnosticEngine& diagnostics_;
  std::span<const ImportedPackageView> imported_packages_;
  SemanticModel model_;
  std::map<std::string, FileId, std::less<>> imported_file_ids_;
  std::map<std::string, TypeId, std::less<>> imported_type_ids_;
  std::map<std::string, SymbolId, std::less<>> imported_member_ids_;
  std::map<std::size_t, std::map<std::string, SymbolId, std::less<>>>
      enum_case_names_;
  FileId current_file_{0};
  TypeId expected_return_type_{0};
  std::optional<SymbolKind> current_callable_kind_;
  std::optional<SymbolId> current_callable_symbol_;
  std::optional<FileId> current_effect_field_;
  std::vector<Scope> scopes_;
  std::vector<std::vector<VisibleFile>> visible_files_;
  std::map<std::string, FileId, std::less<>> prelude_files_;
  NonNullSet active_non_null_;
  std::optional<std::size_t> current_scope_;
  bool has_implicit_receiver_{false};
  bool analyzing_base_initializer_{false};
  bool defer_default_numeric_literal_range_{false};
  bool constant_context_{false};
  std::map<std::string, std::pair<std::size_t, std::size_t>> constant_budgets_;
  std::vector<std::map<std::string, std::vector<SymbolId>, std::less<>>>
      member_names_;
  std::vector<bool> instance_field_initializers_complete_;
  std::vector<SymbolId> incomplete_base_initializers_;
  std::vector<TransferContext> transfers_;
  bool flow_reachable_{true};
  bool effects_reachable_{true};
  SymbolId error_message_symbol_{0};
  SymbolId error_default_constructor_{0};
  SymbolId error_message_constructor_{0};
  SymbolId division_by_zero_constructor_{0};
  std::map<std::size_t, std::vector<ErrorEffectSource>> callable_effects_;
  std::map<std::size_t, std::vector<ErrorEffectSource>> field_effects_;
};

SemanticAnalysisResult analyze_semantics(
    std::span<const FileClassDecl* const> files, DiagnosticEngine& diagnostics,
    std::span<const ImportedPackageView> imported_packages) {
  return SemanticAnalyzer{files, diagnostics, imported_packages}.run();
}

}  // namespace cloth
