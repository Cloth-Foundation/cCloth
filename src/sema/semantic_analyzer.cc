// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/semantic_analyzer.h"

#include "cloth/lexer/literal.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/canonical_identity.h"
#include "cloth/sema/field_initialization_analysis.h"
#include "cloth/sema/numeric_types.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

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
    register_file_classes();
    register_imported_packages();
    register_imports();
    register_type_relationships();
    register_members();
    validate_interface_contracts();
    validate_overrides();
    validate_interface_conformance();
    analyze_definitions();
    return SemanticAnalysisResult{std::move(model_),
                                  !diagnostics_.has_errors()};
  }

 private:
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

      const std::optional<TypeId> existing_type = model_.find_type(syntax.name);
      if (existing_type &&
          model_.type(*existing_type).kind != TypeKind::kFileClass &&
          model_.type(*existing_type).kind != TypeKind::kInterface &&
          model_.type(*existing_type).kind != TypeKind::kEnum) {
        diagnostics_.error(
            point_range(syntax.range.begin),
            "file class name '" + syntax.name + "' conflicts with a core type");
        identity_valid = false;
      }

      TypeId type = model_.error_type();
      if (identity_valid) {
        const TypeKind type_kind =
            syntax.kind == FileTypeKind::kEnum        ? TypeKind::kEnum
            : syntax.kind == FileTypeKind::kInterface ? TypeKind::kInterface
                                                      : TypeKind::kFileClass;
        type = model_.add_type(
            SemanticType{type_kind, syntax.qualified_name, file_id});
      }
      const SymbolId class_symbol = model_.add_symbol(SemanticSymbol{
          syntax.kind == FileTypeKind::kEnum        ? SymbolKind::kEnum
          : syntax.kind == FileTypeKind::kInterface ? SymbolKind::kInterface
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
          syntax.kind == FileTypeKind::kEnum        ? NominalKind::kEnum
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
            imported.kind == FileTypeKind::kEnum        ? TypeKind::kEnum
            : imported.kind == FileTypeKind::kInterface ? TypeKind::kInterface
                                                        : TypeKind::kFileClass;
        const std::string qualified_name =
            qualified_file_name(imported.nominal_identity.package.name,
                                imported.nominal_identity.source_package,
                                imported.nominal_identity.name);
        const TypeId type =
            model_.add_type(SemanticType{type_kind, qualified_name, file_id});
        const SourceRange range = imported_range(imported.location);
        const SymbolId class_symbol = model_.add_symbol(SemanticSymbol{
            imported.kind == FileTypeKind::kEnum ? SymbolKind::kEnum
            : imported.kind == FileTypeKind::kInterface
                ? SymbolKind::kInterface
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
              (model_.type(type->second).kind == TypeKind::kEnum ||
               imported.static_value->kind == LiteralKind::kEnum) &&
              (imported.static_value->kind != LiteralKind::kEnum ||
               !enum_constant_tag(imported.static_value->lexeme, type->second,
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
    if (const std::optional<TypeId> core = model_.find_type(name);
        core && model_.type(*core).kind != TypeKind::kFileClass &&
        model_.type(*core).kind != TypeKind::kInterface &&
        model_.type(*core).kind != TypeKind::kEnum) {
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

  void register_type_relationships() {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      const FileId file_id{index};
      const FileClassDecl& syntax = *files_[index];
      if (syntax.base_class) {
        const TypeId base_type = resolve_type(*syntax.base_class, file_id);
        if (base_type != model_.error_type()) {
          const SemanticType& base = model_.type(base_type);
          if (syntax.kind != FileTypeKind::kClass) {
            diagnostics_.error(syntax.base_class->range,
                               "interfaces cannot inherit a class");
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

  void register_field(FileId file_id, std::size_t index) {
    const FieldDecl& field = files_[file_id.value]->fields.at(index);
    const TypeId type = resolve_type(field.type, file_id);
    const SymbolId symbol = model_.add_symbol(
        SemanticSymbol{SymbolKind::kField,
                       std::string{field.name},
                       type,
                       {},
                       field.visibility,
                       file_id,
                       field.range,
                       field.is_valid && type != model_.error_type()});
    model_.mutable_symbol(symbol).is_final = field.is_final;
    model_.mutable_symbol(symbol).is_static = field.is_static;
    model_.mutable_file(file_id).fields.at(index) = symbol;
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
    if (function.is_final && !function.is_override) {
      diagnostics_.error(function.range,
                         "final function '" + std::string{function.name} +
                             "' must also be declared override");
      model_.mutable_symbol(symbol).is_valid = false;
      model_.mutable_file(file_id).is_valid = false;
    }
    if (function.is_abstract) {
      if (files_[file_id.value]->kind == FileTypeKind::kClass &&
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
    if (files_[file_id.value]->kind == FileTypeKind::kClass &&
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
      const SemanticSymbol& candidate = model_.symbol(candidate_id);
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

      const SemanticSymbol& inherited = model_.symbol(*existing);
      if (is_override_return_compatible(inherited.type, candidate.type)) {
        *existing = candidate_id;
        return;
      }
      if (is_override_return_compatible(candidate.type, inherited.type)) {
        return;
      }
      diagnostics_.error(candidate.range,
                         "interface function '" +
                             callable_signature(candidate) +
                             "' conflicts with inherited return type '" +
                             type_name(inherited.type) + "'");
      diagnostics_.note(inherited.range,
                        "conflicting interface function is declared here");
      model_.mutable_file(interface_file).is_valid = false;
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
    std::vector<bool> complete(files_.size(), false);
    std::size_t remaining = files_.size();
    while (remaining != 0) {
      bool made_progress = false;
      for (std::size_t index = 0; index < files_.size(); ++index) {
        if (complete[index]) {
          continue;
        }
        const FileId file_id{index};
        if (model_.file(file_id).kind == FileTypeKind::kInterface) {
          complete[index] = true;
          --remaining;
          made_progress = true;
          continue;
        }
        const std::optional<FileId> base = model_.file(file_id).base_file;
        if (base && base->value < complete.size() && !complete[base->value]) {
          continue;
        }
        validate_file_overrides(file_id);
        complete[index] = true;
        --remaining;
        made_progress = true;
      }
      if (!made_progress) {
        break;
      }
    }
  }

  void validate_interface_conformance() {
    std::vector<bool> complete(files_.size(), false);
    std::size_t remaining = 0;
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (model_.file(FileId{index}).kind == FileTypeKind::kClass) {
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

    while (remaining != 0) {
      bool made_progress = false;
      for (std::size_t index = 0; index < files_.size(); ++index) {
        const FileId file_id{index};
        FileSemantics& file = model_.mutable_file(file_id);
        if (complete[index] || file.kind != FileTypeKind::kClass) {
          continue;
        }
        if (file.base_file && file.base_file->value < complete.size() &&
            !complete[file.base_file->value]) {
          continue;
        }

        std::vector<FileId> interfaces;
        if (file.base_file) {
          interfaces = model_.file(*file.base_file).interfaces;
        }
        for (const FileId direct : file.direct_interfaces) {
          for (const FileId inherited : model_.file(direct).interfaces) {
            append_interface(interfaces, inherited);
          }
        }

        std::vector<InterfaceImplementation> implementations;
        for (const FileId interface_file : interfaces) {
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
            functions.push_back(*implementation);
          }
          if (complete_contract) {
            implementations.push_back(
                InterfaceImplementation{interface_file, std::move(functions)});
          }
        }
        file.interfaces = std::move(interfaces);
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
        if (symbol.is_override) {
          diagnostics_.error(symbol.range, "function '" + symbol.name +
                                               "' does not override an "
                                               "inherited function");
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
    model_.mutable_file(file_id).constructors.at(index) = symbol;
  }

  TypeId resolve_type(const TypeSyntax& syntax, FileId current_file,
                      bool allow_void = false) {
    if (const std::optional<TypeId> core = model_.find_type(syntax.name);
        core && model_.type(*core).kind != TypeKind::kFileClass &&
        model_.type(*core).kind != TypeKind::kInterface &&
        model_.type(*core).kind != TypeKind::kEnum) {
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
    for (std::size_t file_index = 0; file_index < files_.size(); ++file_index) {
      current_file_ = FileId{file_index};
      const std::size_t diagnostic_begin = diagnostics_.diagnostics().size();
      const FileClassDecl& syntax = *files_[file_index];
      for (const MemberReference& reference : syntax.member_order) {
        switch (reference.kind) {
          case DeclarationKind::kField:
            analyze_field(reference.index);
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
    const FieldDecl& field = files_[current_file_.value]->fields.at(index);
    const SymbolId symbol = model_.file(current_file_).fields.at(index);
    if (field.is_static) {
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
      return;
    }
    begin_root_scope(!field.is_static);
    const TypeId field_type = model_.symbol(symbol).type;
    const ExpressionState value =
        analyze_expression(*field.initializer, field_type);
    if (field.is_static && (!is_static_scalar_initializer(*field.initializer) ||
                            !is_static_scalar_type(field_type))) {
      diagnostics_.error(
          expression_range(*field.initializer),
          "static field initializer must be a scalar literal or enum case");
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

    if (is_constructor) {
      analyze_constructor_initializer(callable_symbol, initializer);
    }

    expected_return_type_ = return_type;
    current_callable_kind_ = model_.symbol(callable_symbol).kind;
    static_cast<void>(analyze_block(body, false));
    current_callable_kind_.reset();
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

  void analyze_constructor_initializer(
      SymbolId constructor_symbol, const ConstructorInitializer* initializer) {
    const FileSemantics& file = model_.file(current_file_);
    if (!file.base_file) {
      if (initializer != nullptr) {
        diagnostics_.error(initializer->range,
                           "root class constructor cannot have a base "
                           "initializer");
      }
      return;
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
      return;
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
    analyzing_base_initializer_ = true;
    for (const ExpressionId argument : initializer->arguments) {
      ExpressionState value = analyze_overload_argument(argument);
      check_value(value, expression_range(argument));
      has_error_argument =
          has_error_argument || value.type == model_.error_type();
      arguments.push_back(std::move(value));
    }
    analyzing_base_initializer_ = false;

    if (initialized_type != base.type) {
      return;
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
      return;
    }
    if (matches.empty()) {
      diagnostics_.error(initializer->range,
                         "no matching base constructor '" + base_name +
                             "' for " + std::to_string(arguments.size()) +
                             " argument(s)");
      return;
    }
    if (matches.size() > 1) {
      diagnostics_.error(initializer->range,
                         "base constructor call is ambiguous between " +
                             std::to_string(matches.size()) + " overloads");
      return;
    }
    const SymbolId selected = matches.front();
    const SemanticSymbol& selected_symbol = model_.symbol(selected);
    if (selected_symbol.visibility == Visibility::kPrivate &&
        selected_symbol.file != current_file_) {
      diagnostics_.error(initializer->range,
                         "base constructor for '" + base_name + "' is private");
      diagnostics_.note(selected_symbol.range,
                        "private constructor is declared here");
      return;
    }
    apply_overload_argument_context(initializer->arguments, arguments,
                                    selected_symbol.parameter_types);
    model_.mutable_symbol(constructor_symbol).base_constructor = selected;
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

  bool analyze_block(BlockId id, bool create_scope) {
    if (create_scope) {
      push_scope();
    }
    bool definitely_returns = false;
    const Block& block = files_[current_file_.value]->storage.block(id);
    for (const StatementId statement : block.statements) {
      const bool statement_returns = analyze_statement(statement);
      definitely_returns = definitely_returns || statement_returns;
    }
    if (create_scope) {
      pop_scope();
    }
    return definitely_returns;
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
      } else if (initializer->type != model_.void_type()) {
        type = initializer->type;
      }
      if (!initializer && !local->is_final &&
          model_.type(type).kind == TypeKind::kEnum) {
        diagnostics_.error(statement.range, "enum local '" +
                                                std::string{local->name} +
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
      return false;
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
        if (expected_return_type_ == model_.void_type()) {
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
      return false;
    }
    if (const auto* if_statement = std::get_if<IfStatement>(&statement.data)) {
      const ExpressionState condition =
          analyze_expression(if_statement->condition);
      check_condition(condition, if_statement->condition, "if condition");

      const ConditionFacts facts = condition_facts(if_statement->condition);
      const NonNullSet branch_base = active_non_null_;
      add_non_null_facts(facts.when_true);
      const bool then_returns = analyze_block(if_statement->then_block, true);
      const NonNullSet then_state = active_non_null_;

      active_non_null_ = branch_base;
      add_non_null_facts(facts.when_false);
      bool else_returns = false;
      if (if_statement->else_block) {
        else_returns = analyze_block(*if_statement->else_block, true);
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
      return then_returns && if_statement->else_block && else_returns;
    }
    if (const auto* while_statement =
            std::get_if<WhileStatement>(&statement.data)) {
      const ExpressionState condition =
          analyze_expression(while_statement->condition);
      check_condition(condition, while_statement->condition, "while condition");
      const ConditionFacts facts = condition_facts(while_statement->condition);
      const NonNullSet loop_base = active_non_null_;
      add_non_null_facts(facts.when_true);
      ++loop_depth_;
      const bool body_returns = analyze_block(while_statement->body, true);
      --loop_depth_;
      if (body_returns) {
        active_non_null_ = loop_base;
      } else {
        active_non_null_ = intersect_symbols(loop_base, active_non_null_);
      }
      return false;
    }
    if (const auto* for_statement =
            std::get_if<ForEachStatement>(&statement.data)) {
      const ExpressionState iterable =
          analyze_expression(for_statement->iterable);
      check_value(iterable, expression_range(for_statement->iterable));

      TypeId element_type = model_.error_type();
      if (iterable.type != model_.error_type()) {
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
      ++loop_depth_;
      const bool body_returns = analyze_block(for_statement->body, false);
      --loop_depth_;
      pop_scope();
      if (body_returns) {
        active_non_null_ = loop_base;
      } else {
        active_non_null_ = intersect_symbols(loop_base, active_non_null_);
      }
      return false;
    }
    if (const auto* for_statement =
            std::get_if<ForStatement>(&statement.data)) {
      push_scope();
      if (for_statement->initializer) {
        static_cast<void>(analyze_statement(*for_statement->initializer));
      }

      ConditionFacts facts;
      if (for_statement->condition) {
        const ExpressionState condition =
            analyze_expression(*for_statement->condition);
        check_condition(condition, *for_statement->condition, "for condition");
        facts = condition_facts(*for_statement->condition);
      }
      const NonNullSet loop_base = active_non_null_;
      add_non_null_facts(facts.when_true);

      ++loop_depth_;
      const bool body_returns = analyze_block(for_statement->body, true);
      --loop_depth_;
      for (const ExpressionId update : for_statement->updates) {
        static_cast<void>(analyze_expression(update));
      }
      pop_scope();
      if (body_returns) {
        active_non_null_ = loop_base;
      } else {
        active_non_null_ = intersect_symbols(loop_base, active_non_null_);
      }
      return false;
    }
    if (std::holds_alternative<BreakStatement>(statement.data)) {
      if (loop_depth_ == 0) {
        diagnostics_.error(statement.range,
                           "'break' is only valid inside a loop");
      }
      return false;
    }
    if (std::holds_alternative<ContinueStatement>(statement.data)) {
      if (loop_depth_ == 0) {
        diagnostics_.error(statement.range,
                           "'continue' is only valid inside a loop");
      }
      return false;
    }
    const auto& nested = std::get<NestedBlockStatement>(statement.data);
    return analyze_block(nested.block, true);
  }

  ExpressionState analyze_expression(
      ExpressionId id, std::optional<TypeId> expected = std::nullopt,
      bool is_negated_literal = false) {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    ExpressionState state{model_.error_type()};

    if (std::holds_alternative<InvalidExpression>(expression.data)) {
      return record_expression(id, std::move(state));
    }
    if (const auto* identifier =
            std::get_if<IdentifierExpression>(&expression.data)) {
      state = analyze_identifier(*identifier, expression.range);
    } else if (std::holds_alternative<SuperExpression>(expression.data)) {
      state = analyze_super(expression.range);
    } else if (const auto* literal =
                   std::get_if<LiteralExpression>(&expression.data)) {
      state = analyze_literal(*literal, expression.range, expected,
                              is_negated_literal);
    } else if (const auto* unary =
                   std::get_if<UnaryExpression>(&expression.data)) {
      state = analyze_unary(*unary, expression.range, expected);
    } else if (const auto* update =
                   std::get_if<UpdateExpression>(&expression.data)) {
      state = analyze_update(*update, expression.range);
    } else if (const auto* binary =
                   std::get_if<BinaryExpression>(&expression.data)) {
      state = analyze_binary(*binary, expression.range, expected);
    } else if (const auto* test =
                   std::get_if<TypeTestExpression>(&expression.data)) {
      state = analyze_type_test(*test, expression.range);
    } else if (const auto* cast =
                   std::get_if<CheckedCastExpression>(&expression.data)) {
      state = analyze_checked_cast(*cast, expression.range);
    } else if (const auto* conversion =
                   std::get_if<NumericConversionExpression>(&expression.data)) {
      state = analyze_numeric_conversion(*conversion, expression.range);
    } else if (const auto* assignment =
                   std::get_if<AssignmentExpression>(&expression.data)) {
      state = analyze_assignment(*assignment, expression.range);
    } else if (const auto* member =
                   std::get_if<MemberAccessExpression>(&expression.data)) {
      state = analyze_member_access(*member, expression.range);
    } else if (const auto* meta =
                   std::get_if<MetaAccessExpression>(&expression.data)) {
      state = analyze_meta_access(*meta, expression.range);
    } else if (const auto* member =
                   std::get_if<SafeMemberAccessExpression>(&expression.data)) {
      state = analyze_safe_member_access(*member, expression.range);
    } else if (const auto* coalesce =
                   std::get_if<NullCoalesceExpression>(&expression.data)) {
      state = analyze_null_coalesce(*coalesce, expression.range);
    } else if (const auto* assertion =
                   std::get_if<NullAssertExpression>(&expression.data)) {
      state = analyze_null_assert(*assertion, expression.range);
    } else if (const auto* call =
                   std::get_if<CallExpression>(&expression.data)) {
      state = analyze_call(*call, expression.range);
    } else if (const auto* array =
                   std::get_if<ArrayLiteralExpression>(&expression.data)) {
      state = analyze_array_literal(*array, expression.range, expected);
    } else if (const auto* index =
                   std::get_if<IndexExpression>(&expression.data)) {
      state = analyze_index(*index, expression.range);
    } else {
      const auto& parenthesized =
          std::get<ParenthesizedExpression>(expression.data);
      state = analyze_expression(parenthesized.expression, expected,
                                 is_negated_literal);
    }
    return record_expression(id, std::move(state));
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
      const ValueCategory category = symbol.kind == SymbolKind::kSelf
                                         ? ValueCategory::kValue
                                         : ValueCategory::kMutableLocation;
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
        return ExpressionState{
            first.type, ValueCategory::kMutableLocation, members.front(), {}};
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

    if (const std::optional<FileId> inaccessible =
            find_inaccessible_file(current_file_, identifier.name)) {
      diagnostics_.error(
          range, "file class '" +
                     model_.symbol(model_.file(*inaccessible).symbol).name +
                     "' is private");
      return ExpressionState{model_.error_type()};
    }

    std::vector<SymbolId> intrinsics = model_.find_intrinsics(identifier.name);
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
    switch (literal.kind) {
      case LiteralKind::kInteger: {
        TypeId type = *model_.find_type("int");
        if (expected && is_integer(*expected)) {
          type = *expected;
        }
        if ((!defer_default_numeric_literal_range_ || expected) &&
            !integer_literal_fits(literal.lexeme, type, is_negated)) {
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
        if (expected && is_floating_point(*expected)) {
          type = *expected;
        }
        if ((!defer_default_numeric_literal_range_ || expected) &&
            !floating_literal_fits(literal.lexeme, type)) {
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
      ExpressionState state = analyze_expression(element, contextual_element);
      check_value(state, expression_range(element));
      contains_null = contains_null || state.type == model_.null_type();
      if (state.type != model_.error_type() &&
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
    return ExpressionState{model_.get_array_type(*element_type),
                           ValueCategory::kValue};
  }

  ExpressionState analyze_index(const IndexExpression& index,
                                SourceRange range) {
    const ExpressionState object = analyze_expression(index.object);
    const ExpressionState subscript = analyze_expression(index.index);
    check_value(object, expression_range(index.object));
    check_value(subscript, expression_range(index.index));
    if (subscript.type != model_.error_type()) {
      check_assignment(*model_.find_type("int32"), subscript.type,
                       expression_range(index.index), "array index");
    }
    if (object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
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

  ExpressionState analyze_unary(const UnaryExpression& unary, SourceRange range,
                                std::optional<TypeId> expected) {
    const bool numeric_sign = unary.operation == TokenKind::kPlus ||
                              unary.operation == TokenKind::kMinus;
    const ExpressionState operand = analyze_expression(
        unary.operand, numeric_sign ? expected : std::nullopt,
        unary.operation == TokenKind::kMinus);
    if (!check_value(operand, expression_range(unary.operand)) ||
        operand.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
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

  ExpressionState analyze_binary(const BinaryExpression& binary,
                                 SourceRange range,
                                 std::optional<TypeId> expected) {
    const bool is_shift = binary.operation == TokenKind::kShiftLeft ||
                          binary.operation == TokenKind::kShiftRight;
    const std::optional<TypeId> numeric_context =
        expected && is_numeric(*expected) ? expected : std::nullopt;
    ExpressionState left = analyze_expression(binary.left, numeric_context);
    const bool is_short_circuit =
        binary.operation == TokenKind::kAmpersandAmpersand ||
        binary.operation == TokenKind::kPipePipe;
    const bool left_is_boolean =
        left.type == model_.bool_type() ||
        (is_short_circuit && mark_presence_test(left, binary.left));
    const NonNullSet right_base = active_non_null_;
    if (is_short_circuit) {
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
    ExpressionState right = analyze_expression(binary.right, right_context);
    const bool right_is_boolean =
        right.type == model_.bool_type() ||
        (is_short_circuit && mark_presence_test(right, binary.right));
    if (is_short_circuit) {
      active_non_null_ = intersect_symbols(right_base, active_non_null_);
    }
    const bool values = check_value(left, expression_range(binary.left)) &&
                        check_value(right, expression_range(binary.right));
    if (!values || left.type == model_.error_type() ||
        right.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
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
          return ExpressionState{*common, ValueCategory::kValue};
        }
        break;
      case TokenKind::kPercent:
        if (const std::optional<TypeId> common =
                common_binary_numeric_type(binary, left, right);
            common && is_integer(*common)) {
          return ExpressionState{*common, ValueCategory::kValue};
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
    const ExpressionState value =
        is_literal ? analyze_overload_argument(conversion.value)
                   : analyze_expression(conversion.value);
    check_value(value, expression_range(conversion.value));
    if (value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (!is_numeric(value.type)) {
      diagnostics_.error(
          range, "numeric conversion requires a numeric value; found '" +
                     type_name(value.type) + "'");
      return ExpressionState{model_.error_type()};
    }

    if (is_literal) {
      if (!numeric_literal_conversion_fits(conversion.value, target)) {
        diagnostics_.error(
            range,
            "numeric literal is out of range for explicit conversion "
            "to '" +
                type_name(target) + "'");
        return ExpressionState{model_.error_type()};
      }
      static_cast<void>(
          contextualize_numeric_conversion(conversion.value, target));
    }
    return ExpressionState{target, ValueCategory::kValue};
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
        target_kind != TypeKind::kInterface) {
      diagnostics_.error(
          range, "type '" + type_name(target) + "' is not runtime-checkable");
      return false;
    }
    if (source == model_.null_type() || source_kind == TypeKind::kObject ||
        target_kind == TypeKind::kObject || source_base == target) {
      return true;
    }
    if (source_kind == TypeKind::kFileClass &&
        target_kind == TypeKind::kFileClass &&
        (is_file_class_subtype(source_base, target) ||
         is_file_class_subtype(target, source_base))) {
      return true;
    }
    if (source_kind == TypeKind::kInterface &&
        target_kind == TypeKind::kInterface) {
      return true;
    }
    if (source_kind == TypeKind::kFileClass &&
        target_kind == TypeKind::kInterface) {
      const SemanticType& source_info = model_.type(source_base);
      if (implements_interface(source_base, target) ||
          (source_info.file && !model_.file(*source_info.file).is_sealed)) {
        return true;
      }
    }
    if (source_kind == TypeKind::kInterface &&
        target_kind == TypeKind::kFileClass) {
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
    const ExpressionState value = analyze_expression(
        assignment.value,
        is_shift_assignment ? std::nullopt : std::optional{target.type});
    if (target.type != model_.error_type() &&
        target.category != ValueCategory::kMutableLocation) {
      diagnostics_.error(expression_range(assignment.target),
                         "assignment target is not mutable");
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
    if (target.symbol) {
      invalidate_narrowing(*target.symbol);
    }
    if (target.type == model_.error_type() ||
        value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }

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
    }
    return ExpressionState{
        target.type, ValueCategory::kValue, target.symbol, {}};
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
    const SemanticType& object_type = model_.type(object.type);
    if (object_type.kind == TypeKind::kEnum && object_type.file) {
      if (object.category != ValueCategory::kType) {
        diagnostics_.error(range,
                           "enum cases must be accessed through their type");
        return ExpressionState{model_.error_type()};
      }
      for (const SymbolId id : model_.file(*object_type.file).enum_cases) {
        if (model_.symbol(id).name == member.member) {
          return ExpressionState{object.type, ValueCategory::kValue, id};
        }
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
    if ((object_type.kind != TypeKind::kFileClass &&
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
      return ExpressionState{
          first.type, ValueCategory::kMutableLocation, members.front(), {}};
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
    if (!check_value(object, expression_range(meta.object)) ||
        object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }

    const SemanticType& object_type = model_.type(object.type);
    if (object_type.kind == TypeKind::kNullable) {
      diagnostics_.error(range, "nullable type '" + object_type.name +
                                    "' has no meta queries without narrowing");
      return ExpressionState{model_.error_type()};
    }
    if (meta.meta == "typeName" &&
        (is_reference(object.type) || object_type.kind == TypeKind::kEnum)) {
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
    if ((object_type.kind != TypeKind::kFileClass &&
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
    const ExpressionState fallback = analyze_expression(coalesce.fallback);
    check_value(fallback, expression_range(coalesce.fallback));
    active_non_null_ = intersect_symbols(fallback_base, active_non_null_);

    if (nullable.type == model_.error_type() ||
        fallback.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
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
    for (const ExpressionId argument : call.arguments) {
      ExpressionState value = analyze_overload_argument(argument);
      check_value(value, expression_range(argument));
      has_error_argument =
          has_error_argument || value.type == model_.error_type();
      arguments.push_back(std::move(value));
    }

    if (callee.integer_meta_operation) {
      return analyze_integer_meta_call(*callee.integer_meta_operation, call,
                                       arguments, range);
    }

    std::vector<SymbolId> candidates = callee.candidates;
    if (callee.category == ValueCategory::kType) {
      const SemanticType& type = model_.type(callee.type);
      if (type.kind == TypeKind::kFileClass && type.file) {
        const FileSemantics& target = model_.file(*type.file);
        if (target.is_abstract) {
          diagnostics_.error(range, "abstract file class '" + type.name +
                                        "' cannot be constructed");
          return ExpressionState{model_.error_type()};
        }
        candidates = target.constructors;
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

  bool is_static_scalar_initializer(ExpressionId id) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&expression.data)) {
      return is_static_scalar_initializer(grouped->expression);
    }
    if (const auto* conversion =
            std::get_if<NumericConversionExpression>(&expression.data)) {
      return is_numeric_literal_expression(conversion->value);
    }
    const auto symbol =
        model_.file(current_file_).expressions.at(id.value).symbol;
    if (symbol && model_.symbol(*symbol).kind == SymbolKind::kEnumCase)
      return true;
    const auto* literal = std::get_if<LiteralExpression>(&expression.data);
    return literal != nullptr && literal->kind != LiteralKind::kString &&
           literal->kind != LiteralKind::kNull;
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
                            state.integer_meta_operation};
    return state;
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
    for (const SymbolId symbol_id : file.fields) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.name == name) {
        matches.push_back(symbol_id);
      }
    }
    for (const SymbolId symbol_id : file.functions) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.name == name) {
        matches.push_back(symbol_id);
      }
    }
    return matches;
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
    if (expected_type.kind == TypeKind::kFileClass &&
        actual_type.kind == TypeKind::kFileClass &&
        is_file_class_subtype(actual, expected)) {
      return true;
    }
    if (expected_type.kind == TypeKind::kInterface &&
        (actual_type.kind == TypeKind::kFileClass ||
         actual_type.kind == TypeKind::kInterface) &&
        implements_interface(actual, expected)) {
      return true;
    }
    if (expected_type.kind == TypeKind::kObject) {
      return actual_type.kind == TypeKind::kString ||
             actual_type.kind == TypeKind::kFileClass ||
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
    if (subtype_info.kind != TypeKind::kFileClass || !subtype_info.file ||
        supertype_info.kind != TypeKind::kFileClass || !supertype_info.file) {
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
           kind == TypeKind::kFileClass || kind == TypeKind::kInterface ||
           kind == TypeKind::kArray || kind == TypeKind::kNullable;
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
      std::uint64_t value = 0;
      const char* const begin = literal->lexeme.data();
      const char* const end = begin + literal->lexeme.size();
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
    const char* const begin = lexeme.data();
    const char* const end = begin + lexeme.size();
    if (kind == TypeKind::kFloat32) {
      float value = 0.0F;
      const auto parsed =
          std::from_chars(begin, end, value, std::chars_format::general);
      return parsed.ec == std::errc{} && parsed.ptr == end &&
             std::isfinite(value);
    }
    if (kind == TypeKind::kFloat64) {
      double value = 0.0;
      const auto parsed =
          std::from_chars(begin, end, value, std::chars_format::general);
      return parsed.ec == std::errc{} && parsed.ptr == end &&
             std::isfinite(value);
    }
    return false;
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

  bool numeric_literal_expression_fits(ExpressionId id, TypeId target,
                                       bool is_negated = false) const {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    if (const auto* literal =
            std::get_if<LiteralExpression>(&expression.data)) {
      if (literal->kind == LiteralKind::kInteger && is_integer(target)) {
        return integer_literal_fits(literal->lexeme, target, is_negated);
      }
      return literal->kind == LiteralKind::kFloat &&
             is_floating_point(target) &&
             floating_literal_fits(literal->lexeme, target);
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
      if (literal->kind == LiteralKind::kInteger) {
        if (is_integer(target)) {
          return integer_literal_fits(literal->lexeme, target, is_negated);
        }
        std::uint64_t value = 0;
        const char* const begin = literal->lexeme.data();
        const char* const end = begin + literal->lexeme.size();
        const auto parsed = std::from_chars(begin, end, value);
        return is_floating_point(target) && parsed.ec == std::errc{} &&
               parsed.ptr == end;
      }
      if (literal->kind != LiteralKind::kFloat) {
        return false;
      }
      if (is_floating_point(target)) {
        return floating_literal_fits(literal->lexeme, target);
      }
      if (!is_integer(target)) {
        return false;
      }

      double parsed_value = 0.0;
      const char* const begin = literal->lexeme.data();
      const char* const end = begin + literal->lexeme.size();
      const auto parsed =
          std::from_chars(begin, end, parsed_value, std::chars_format::general);
      if (parsed.ec != std::errc{} || parsed.ptr != end ||
          !std::isfinite(parsed_value)) {
        return false;
      }
      long double value = static_cast<long double>(parsed_value);
      if (is_negated) {
        value = -value;
      }
      value = std::trunc(value);
      const NumericTypeProperties properties =
          *numeric_type_properties(model_.type(target).kind);
      if (properties.category == NumericCategory::kUnsignedInteger) {
        const long double upper =
            std::ldexp(1.0L, static_cast<int>(properties.bit_width));
        return value >= 0.0L && value < upper;
      }
      const long double upper =
          std::ldexp(1.0L, static_cast<int>(properties.bit_width - 1));
      return value >= -upper && value < upper;
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
        previous || is_numeric_literal_expression(id);
    ExpressionState state = analyze_expression(id);
    defer_default_numeric_literal_range_ = previous;
    return state;
  }

  bool overload_argument_matches(ExpressionId id,
                                 const ExpressionState& argument,
                                 TypeId parameter, bool& is_exact) const {
    is_exact = false;
    if (argument.type == model_.error_type() ||
        parameter == model_.error_type()) {
      return true;
    }
    if (is_numeric_literal_expression(id)) {
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
  FileId current_file_{0};
  TypeId expected_return_type_{0};
  std::optional<SymbolKind> current_callable_kind_;
  std::vector<Scope> scopes_;
  std::vector<std::vector<VisibleFile>> visible_files_;
  NonNullSet active_non_null_;
  std::optional<std::size_t> current_scope_;
  bool has_implicit_receiver_{false};
  bool analyzing_base_initializer_{false};
  bool defer_default_numeric_literal_range_{false};
  std::size_t loop_depth_{0};
};

SemanticAnalysisResult analyze_semantics(
    std::span<const FileClassDecl* const> files, DiagnosticEngine& diagnostics,
    std::span<const ImportedPackageView> imported_packages) {
  return SemanticAnalyzer{files, diagnostics, imported_packages}.run();
}

}  // namespace cloth
