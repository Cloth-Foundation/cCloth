#include "cloth/sema/semantic_analyzer.h"

#include "cloth/lexer/token.h"
#include "cloth/sema/field_initialization_analysis.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

}  // namespace

class SemanticAnalyzer {
 public:
  SemanticAnalyzer(std::span<const FileClassDecl* const> files,
                   DiagnosticEngine& diagnostics)
      : files_(files), diagnostics_(diagnostics) {}

  SemanticAnalysisResult run() {
    register_file_classes();
    register_object_print_intrinsics();
    register_imports();
    register_members();
    analyze_definitions();
    return SemanticAnalysisResult{std::move(model_),
                                  !diagnostics_.has_errors()};
  }

 private:
  void register_object_print_intrinsics() {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      const TypeId type = model_.file(FileId{index}).type;
      if (type == model_.error_type()) {
        continue;
      }
      model_.add_intrinsic("print", {type}, IntrinsicKind::kPrintObject);
      model_.add_intrinsic("println", {type}, IntrinsicKind::kPrintObject);
    }
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

      const std::optional<TypeId> existing_type = model_.find_type(syntax.name);
      if (existing_type &&
          model_.type(*existing_type).kind != TypeKind::kFileClass) {
        diagnostics_.error(
            point_range(syntax.range.begin),
            "file class name '" + syntax.name + "' conflicts with a core type");
        identity_valid = false;
      }

      TypeId type = model_.error_type();
      if (identity_valid) {
        type = model_.add_type(
            SemanticType{TypeKind::kFileClass, syntax.qualified_name, file_id});
      }
      const SymbolId class_symbol =
          model_.add_symbol(SemanticSymbol{SymbolKind::kFileClass,
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
      static_cast<void>(model_.add_file(std::move(file)));
    }
    visible_files_.resize(files_.size());
  }

  void register_imports() {
    for (std::size_t current_index = 0; current_index < files_.size();
         ++current_index) {
      const FileId current_file{current_index};
      const FileClassDecl& current = *files_[current_index];
      for (std::size_t target_index = 0; target_index < files_.size();
           ++target_index) {
        const FileClassDecl& target = *files_[target_index];
        if (target.package_name != current.package_name) {
          continue;
        }
        const FileId target_file{target_index};
        const SemanticSymbol& symbol =
            model_.symbol(model_.file(target_file).symbol);
        if (target_file != current_file &&
            symbol.visibility == Visibility::kPrivate) {
          continue;
        }
        visible_files_[current_index].push_back(
            VisibleFile{target.name, target_file, VisibleFileKind::kSamePackage,
                        target.range});
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
    const std::string target_name =
        import.package_name.empty()
            ? import.type_name
            : import.package_name + '.' + import.type_name;
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
    for (std::size_t index = 0; index < files_.size(); ++index) {
      const FileClassDecl& target = *files_[index];
      if (target.package_name != import.package_name) {
        continue;
      }
      found_package = true;
      const FileId target_file{index};
      const SemanticSymbol& symbol =
          model_.symbol(model_.file(target_file).symbol);
      if (symbol.visibility == Visibility::kPrivate) {
        continue;
      }
      bind_visible_file(current_file, target.name, target_file,
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
        core && model_.type(*core).kind != TypeKind::kFileClass) {
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
    if (function.name == "Main" && !function.is_static) {
      diagnostics_.error(function.range,
                         "entry point 'Main' must be declared static");
      model_.mutable_symbol(symbol).is_valid = false;
    }
    model_.mutable_file(file_id).functions.at(index) = symbol;
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
        core && model_.type(*core).kind != TypeKind::kFileClass) {
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
      if (const std::optional<FileId> inaccessible =
              find_inaccessible_file(current_file, syntax.name)) {
        diagnostics_.error(syntax.range,
                           "file class '" +
                               files_[inaccessible->value]->qualified_name +
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
      } else if (!is_static_scalar_initializer(*field.initializer) ||
                 !is_static_scalar_type(model_.symbol(symbol).type)) {
        diagnostics_.error(expression_range(*field.initializer),
                           "static field initializer must be a scalar literal");
      }
    }
    if (!field.initializer) {
      return;
    }
    begin_root_scope(!field.is_static);
    const ExpressionState value = analyze_expression(*field.initializer);
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
    analyze_callable(symbol, function.parameters, function.body,
                     model_.symbol(symbol).type);
  }

  void analyze_constructor(std::size_t index) {
    const ConstructorDecl& constructor =
        files_[current_file_.value]->constructors.at(index);
    const SymbolId symbol = model_.file(current_file_).constructors.at(index);
    analyze_callable(symbol, constructor.parameters, constructor.body,
                     model_.void_type());
  }

  void analyze_callable(SymbolId callable_symbol,
                        std::span<const ParameterDecl> parameters, BlockId body,
                        TypeId return_type) {
    const SemanticSymbol& callable = model_.symbol(callable_symbol);
    begin_root_scope(callable.kind == SymbolKind::kConstructor ||
                     !callable.is_static);
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

    expected_return_type_ = return_type;
    current_callable_kind_ = model_.symbol(callable_symbol).kind;
    static_cast<void>(analyze_block(body, false));
    current_callable_kind_.reset();
    expected_return_type_ = model_.void_type();
    end_root_scope();
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
      TypeId type = model_.error_type();
      std::optional<ExpressionState> initializer;
      if (local->initializer) {
        initializer = analyze_expression(*local->initializer);
        check_value(*initializer, expression_range(*local->initializer));
      }
      if (local->type) {
        type = resolve_type(*local->type, current_file_);
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
        const ExpressionState value =
            analyze_expression(*return_statement->value);
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
            std::get_if<ForStatement>(&statement.data)) {
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

  ExpressionState analyze_expression(ExpressionId id) {
    const Expression& expression =
        files_[current_file_.value]->storage.expression(id);
    ExpressionState state{model_.error_type()};

    if (std::holds_alternative<InvalidExpression>(expression.data)) {
      return record_expression(id, std::move(state));
    }
    if (const auto* identifier =
            std::get_if<IdentifierExpression>(&expression.data)) {
      state = analyze_identifier(*identifier, expression.range);
    } else if (const auto* literal =
                   std::get_if<LiteralExpression>(&expression.data)) {
      state = analyze_literal(*literal);
    } else if (const auto* unary =
                   std::get_if<UnaryExpression>(&expression.data)) {
      state = analyze_unary(*unary, expression.range);
    } else if (const auto* binary =
                   std::get_if<BinaryExpression>(&expression.data)) {
      state = analyze_binary(*binary, expression.range);
    } else if (const auto* assignment =
                   std::get_if<AssignmentExpression>(&expression.data)) {
      state = analyze_assignment(*assignment, expression.range);
    } else if (const auto* member =
                   std::get_if<MemberAccessExpression>(&expression.data)) {
      state = analyze_member_access(*member, expression.range);
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
      state = analyze_array_literal(*array, expression.range);
    } else if (const auto* index =
                   std::get_if<IndexExpression>(&expression.data)) {
      state = analyze_index(*index, expression.range);
    } else {
      const auto& parenthesized =
          std::get<ParenthesizedExpression>(expression.data);
      state = analyze_expression(parenthesized.expression);
    }
    return record_expression(id, std::move(state));
  }

  ExpressionState analyze_identifier(const IdentifierExpression& identifier,
                                     SourceRange range) {
    if (const std::optional<SymbolId> local = lookup_local(identifier.name)) {
      const SemanticSymbol& symbol = model_.symbol(*local);
      const ValueCategory category = symbol.kind == SymbolKind::kSelf
                                         ? ValueCategory::kValue
                                         : ValueCategory::kMutableLocation;
      return ExpressionState{
          narrowed_type(*local).value_or(symbol.type), category, *local, {}};
    }

    std::vector<SymbolId> members =
        find_members(current_file_, identifier.name, false);
    if (!members.empty()) {
      const SemanticSymbol& first = model_.symbol(members.front());
      if (first.kind == SymbolKind::kField) {
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
          range, "file class '" + files_[inaccessible->value]->qualified_name +
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

  ExpressionState analyze_literal(const LiteralExpression& literal) const {
    switch (literal.kind) {
      case LiteralKind::kInteger:
        return ExpressionState{*model_.find_type("int"), ValueCategory::kValue};
      case LiteralKind::kFloat:
        return ExpressionState{*model_.find_type("float64"),
                               ValueCategory::kValue};
      case LiteralKind::kString:
        return ExpressionState{model_.string_type(), ValueCategory::kValue};
      case LiteralKind::kCharacter:
        return ExpressionState{*model_.find_type("char"),
                               ValueCategory::kValue};
      case LiteralKind::kBoolean:
        return ExpressionState{model_.bool_type(), ValueCategory::kValue};
      case LiteralKind::kNull:
        return ExpressionState{model_.null_type(), ValueCategory::kValue};
    }
    return ExpressionState{model_.error_type()};
  }

  ExpressionState analyze_array_literal(const ArrayLiteralExpression& array,
                                        SourceRange range) {
    std::vector<ExpressionState> elements;
    elements.reserve(array.elements.size());
    std::optional<TypeId> element_type;
    bool contains_null = false;
    for (const ExpressionId element : array.elements) {
      ExpressionState state = analyze_expression(element);
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
      const SemanticType& type = model_.type(element.type);
      if (type.kind == TypeKind::kNullable &&
          type.element_type == element_type) {
        element_type = element.type;
      }
    }
    if (contains_null && is_reference(*element_type) &&
        model_.type(*element_type).kind != TypeKind::kNullable) {
      element_type = model_.get_nullable_type(*element_type);
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

  ExpressionState analyze_unary(const UnaryExpression& unary,
                                SourceRange range) {
    const ExpressionState operand = analyze_expression(unary.operand);
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

  ExpressionState analyze_binary(const BinaryExpression& binary,
                                 SourceRange range) {
    const ExpressionState left = analyze_expression(binary.left);
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
    const ExpressionState right = analyze_expression(binary.right);
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
      case TokenKind::kMinus:
      case TokenKind::kStar:
      case TokenKind::kSlash:
        if (left.type == right.type && is_numeric(left.type)) {
          return ExpressionState{left.type, ValueCategory::kValue};
        }
        break;
      case TokenKind::kPercent:
        if (left.type == right.type && is_integer(left.type)) {
          return ExpressionState{left.type, ValueCategory::kValue};
        }
        break;
      case TokenKind::kLess:
      case TokenKind::kLessEqual:
      case TokenKind::kGreater:
      case TokenKind::kGreaterEqual:
        if (left.type == right.type && is_numeric(left.type)) {
          return ExpressionState{model_.bool_type(), ValueCategory::kValue};
        }
        break;
      case TokenKind::kEqualEqual:
      case TokenKind::kBangEqual:
        if (is_assignable(left.type, right.type) ||
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
    const ExpressionState value = analyze_expression(assignment.value);
    if (target.type != model_.error_type() &&
        target.category != ValueCategory::kMutableLocation) {
      diagnostics_.error(expression_range(assignment.target),
                         "assignment target is not mutable");
    }
    if (target.symbol && model_.symbol(*target.symbol).is_final &&
        !can_initialize_final_field(assignment.target, *target.symbol)) {
      const SemanticSymbol& symbol = model_.symbol(*target.symbol);
      diagnostics_.error(expression_range(assignment.target),
                         "cannot assign to final " +
                             std::string{symbol_kind_name(symbol.kind)} + " '" +
                             symbol.name + "'");
    }
    check_value(value, expression_range(assignment.value));
    check_assignment(target.type, value.type, range, "assignment");
    if (target.symbol) {
      invalidate_narrowing(*target.symbol);
    }
    if (target.type == model_.error_type() ||
        value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
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
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.data)) {
      return expression_assigns_symbol(binary->left, symbol) ||
             expression_assigns_symbol(binary->right, symbol);
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
    if (object_type.kind == TypeKind::kNullable) {
      diagnostics_.error(range, "nullable type '" + object_type.name +
                                    "' has no members without narrowing");
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind == TypeKind::kArray) {
      if (member.member == "Length") {
        return ExpressionState{*model_.find_type("int32"),
                               ValueCategory::kValue};
      }
      diagnostics_.error(range, "array type '" + object_type.name +
                                    "' has no member '" +
                                    std::string{member.member} + "'");
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind != TypeKind::kFileClass || !object_type.file) {
      diagnostics_.error(
          range, "type '" + object_type.name + "' has no Cloth members");
      return ExpressionState{model_.error_type()};
    }

    const FileId target_file = *object_type.file;
    std::vector<SymbolId> members =
        find_members(target_file, member.member, true);
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

    const SemanticSymbol& first = model_.symbol(members.front());
    if (first.kind == SymbolKind::kField) {
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
    return ExpressionState{
        model_.error_type(), ValueCategory::kCallable, {}, std::move(members)};
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
        diagnostics_.error(
            range,
            "safe access to value-type member 'Length' requires nullable "
            "value types");
      } else {
        diagnostics_.error(range, "array type '" + object_type.name +
                                      "' has no member '" +
                                      std::string{member.member} + "'");
      }
      return ExpressionState{model_.error_type()};
    }
    if (object_type.kind != TypeKind::kFileClass || !object_type.file) {
      diagnostics_.error(
          range, "type '" + object_type.name + "' has no Cloth members");
      return ExpressionState{model_.error_type()};
    }

    const FileId target_file = *object_type.file;
    std::vector<SymbolId> members =
        find_members(target_file, member.member, true);
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
      ExpressionState value = analyze_expression(argument);
      check_value(value, expression_range(argument));
      has_error_argument =
          has_error_argument || value.type == model_.error_type();
      arguments.push_back(std::move(value));
    }

    std::vector<SymbolId> candidates = callee.candidates;
    if (callee.category == ValueCategory::kType) {
      const SemanticType& type = model_.type(callee.type);
      if (type.kind == TypeKind::kFileClass && type.file) {
        candidates = model_.file(*type.file).constructors;
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
        if (!is_assignable(symbol.parameter_types[index],
                           arguments[index].type)) {
          matches_types = false;
          break;
        }
        matches_exactly = matches_exactly && symbol.parameter_types[index] ==
                                                 arguments[index].type;
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
    return ExpressionState{symbol.type, ValueCategory::kValue, selected, {}};
  }

  bool validate_call_access(ExpressionId callee_id,
                            const SemanticSymbol& callable, SourceRange range) {
    if (callable.kind != SymbolKind::kFunction ||
        callable.intrinsic != IntrinsicKind::kNone) {
      return true;
    }
    const Expression& callee =
        files_[current_file_.value]->storage.expression(callee_id);
    if (const auto* grouped =
            std::get_if<ParenthesizedExpression>(&callee.data)) {
      return validate_call_access(grouped->expression, callable, range);
    }
    const auto* member = std::get_if<MemberAccessExpression>(&callee.data);
    if (member == nullptr) {
      if (!callable.is_static && !has_implicit_receiver_) {
        diagnostics_.error(range, "instance function '" + callable.name +
                                      "' is unavailable in a static context");
        return false;
      }
      return true;
    }

    const ExpressionSemantics& object =
        model_.file(current_file_).expressions.at(member->object.value);
    if (callable.is_static && object.category != ValueCategory::kType) {
      diagnostics_.error(range,
                         "static function '" + callable.name +
                             "' must be accessed through its file class");
      return false;
    }
    if (!callable.is_static && object.category == ValueCategory::kType) {
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
           kind == TypeKind::kFloat64;
  }

  ExpressionState record_expression(ExpressionId id, ExpressionState state) {
    model_.mutable_file(current_file_).expressions.at(id.value) =
        ExpressionSemantics{state.type, state.category, state.symbol};
    return state;
  }

  std::vector<SymbolId> find_members(FileId file_id, std::string_view name,
                                     bool enforce_visibility) const {
    const FileSemantics& file = model_.file(file_id);
    std::vector<SymbolId> matches;
    for (const SymbolId symbol_id : file.fields) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.name == name &&
          (!enforce_visibility || file_id == current_file_ ||
           symbol.visibility == Visibility::kPublic)) {
        matches.push_back(symbol_id);
      }
    }
    for (const SymbolId symbol_id : file.functions) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.name == name &&
          (!enforce_visibility || file_id == current_file_ ||
           symbol.visibility == Visibility::kPublic)) {
        matches.push_back(symbol_id);
      }
    }
    return matches;
  }

  bool has_inaccessible_member(FileId file_id, std::string_view name) const {
    if (file_id == current_file_) {
      return false;
    }
    const FileSemantics& file = model_.file(file_id);
    for (const SymbolId symbol_id : file.fields) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.name == name && symbol.visibility == Visibility::kPrivate) {
        return true;
      }
    }
    for (const SymbolId symbol_id : file.functions) {
      const SemanticSymbol& symbol = model_.symbol(symbol_id);
      if (symbol.name == name && symbol.visibility == Visibility::kPrivate) {
        return true;
      }
    }
    return false;
  }

  std::optional<FileId> find_qualified_file(std::string_view name) const {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (files_[index]->qualified_name == name &&
          model_.file(FileId{index}).type != model_.error_type()) {
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
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (index == current_file.value || files_[index]->name != name ||
          files_[index]->package_name != package_name) {
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
    return expected_type.kind == TypeKind::kNullable &&
           expected_type.element_type &&
           (actual == model_.null_type() ||
            actual == *expected_type.element_type);
  }

  bool is_reference(TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return kind == TypeKind::kString || kind == TypeKind::kFileClass ||
           kind == TypeKind::kArray || kind == TypeKind::kNullable;
  }

  bool is_integer(TypeId type) const {
    switch (model_.type(type).kind) {
      case TypeKind::kByte:
      case TypeKind::kInt8:
      case TypeKind::kInt16:
      case TypeKind::kInt32:
      case TypeKind::kInt64:
      case TypeKind::kUint8:
      case TypeKind::kUint16:
      case TypeKind::kUint32:
      case TypeKind::kUint64:
        return true;
      default:
        return false;
    }
  }

  bool is_numeric(TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return is_integer(type) || kind == TypeKind::kFloat32 ||
           kind == TypeKind::kFloat64;
  }

  void report_operator_type(TokenKind operation, SourceRange range,
                            TypeId operand) {
    diagnostics_.error(
        range, "operator '" + std::string{token_kind_name(operation)} +
                   "' cannot be applied to '" + type_name(operand) + "'");
  }

  std::string type_name(TypeId type) const { return model_.type(type).name; }

  SourceRange expression_range(ExpressionId id) const {
    return files_[current_file_.value]->storage.expression(id).range;
  }

  std::span<const FileClassDecl* const> files_;
  DiagnosticEngine& diagnostics_;
  SemanticModel model_;
  FileId current_file_{0};
  TypeId expected_return_type_{0};
  std::optional<SymbolKind> current_callable_kind_;
  std::vector<Scope> scopes_;
  std::vector<std::vector<VisibleFile>> visible_files_;
  NonNullSet active_non_null_;
  std::optional<std::size_t> current_scope_;
  bool has_implicit_receiver_{false};
  std::size_t loop_depth_{0};
};

SemanticAnalysisResult analyze_semantics(
    std::span<const FileClassDecl* const> files,
    DiagnosticEngine& diagnostics) {
  return SemanticAnalyzer{files, diagnostics}.run();
}

}  // namespace cloth
