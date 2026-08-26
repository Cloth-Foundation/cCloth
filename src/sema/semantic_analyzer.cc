#include "cloth/sema/semantic_analyzer.h"

#include "cloth/lexer/token.h"

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
    register_members();
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
        if (!ascii_case_equal(syntax.name, previous_syntax.name)) {
          continue;
        }
        diagnostics_.error(
            point_range(syntax.range.begin),
            "file class '" + syntax.name +
                "' collides with another source file by ASCII case");
        diagnostics_.note(
            point_range(previous_syntax.range.begin),
            "previous file class is '" + previous_syntax.name + "'");
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
            SemanticType{TypeKind::kFileClass, syntax.name, file_id});
      }
      const SymbolId class_symbol =
          model_.add_symbol(SemanticSymbol{SymbolKind::kFileClass,
                                           syntax.name,
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
    TypeId return_type = model_.no_value_type();
    if (function.return_type) {
      return_type = resolve_type(*function.return_type, file_id);
      valid = valid && return_type != model_.error_type();
    }
    const SymbolId symbol = model_.add_symbol(
        SemanticSymbol{SymbolKind::kFunction, std::string{function.name},
                       return_type, std::move(parameters), function.visibility,
                       file_id, function.range, valid});
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

  TypeId resolve_type(const TypeSyntax& syntax, FileId current_file) {
    const std::optional<TypeId> resolved = model_.find_type(syntax.name);
    if (!resolved) {
      diagnostics_.error(syntax.range,
                         "unknown type '" + std::string{syntax.name} + "'");
      model_.mutable_file(current_file).is_valid = false;
      return model_.error_type();
    }

    const SemanticType& type = model_.type(*resolved);
    if (type.kind == TypeKind::kFileClass && type.file &&
        *type.file != current_file) {
      const SemanticSymbol& class_symbol =
          model_.symbol(model_.file(*type.file).symbol);
      if (class_symbol.visibility == Visibility::kPrivate) {
        diagnostics_.error(syntax.range,
                           "file class '" + type.name + "' is private");
        model_.mutable_file(current_file).is_valid = false;
        return model_.error_type();
      }
    }
    return *resolved;
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
    if (!field.initializer) {
      return;
    }
    begin_root_scope();
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
                     model_.no_value_type());
  }

  void analyze_callable(SymbolId callable_symbol,
                        std::span<const ParameterDecl> parameters, BlockId body,
                        TypeId return_type) {
    begin_root_scope();
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
      model_.mutable_symbol(callable_symbol)
          .parameter_symbols.push_back(symbol);
      bind_name(parameter.name, symbol, parameter.range);
    }

    expected_return_type_ = return_type;
    static_cast<void>(analyze_block(body, false));
    expected_return_type_ = model_.no_value_type();
    end_root_scope();
  }

  void begin_root_scope() {
    scopes_.clear();
    scopes_.push_back(Scope{});
    current_scope_ = 0;
    scopes_[0].entries.push_back(
        ScopeEntry{"self", model_.file(current_file_).self_symbol});
  }

  void end_root_scope() {
    scopes_.clear();
    current_scope_.reset();
  }

  void push_scope() {
    const std::size_t id = scopes_.size();
    scopes_.push_back(Scope{current_scope_, {}});
    current_scope_ = id;
  }

  void pop_scope() { current_scope_ = scopes_.at(*current_scope_).parent; }

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
      const TypeId type = resolve_type(local->type, current_file_);
      if (local->initializer) {
        const ExpressionState value = analyze_expression(*local->initializer);
        check_value(value, expression_range(*local->initializer));
        check_assignment(type, value.type,
                         expression_range(*local->initializer),
                         "local initializer");
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
      model_.mutable_file(current_file_).statement_symbols.at(id.value) =
          symbol;
      bind_name(local->name, symbol, statement.range);
      return false;
    }
    if (const auto* return_statement =
            std::get_if<ReturnStatement>(&statement.data)) {
      if (!return_statement->value) {
        if (expected_return_type_ != model_.no_value_type() &&
            expected_return_type_ != model_.error_type()) {
          diagnostics_.error(statement.range,
                             "return statement requires a value of type '" +
                                 type_name(expected_return_type_) + "'");
        }
      } else {
        const ExpressionState value =
            analyze_expression(*return_statement->value);
        check_value(value, expression_range(*return_statement->value));
        if (expected_return_type_ == model_.no_value_type()) {
          diagnostics_.error(statement.range,
                             "cannot return a value from a no-value callable");
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
      check_value(condition, expression_range(if_statement->condition));
      check_assignment(model_.bool_type(), condition.type,
                       expression_range(if_statement->condition),
                       "if condition");
      const bool then_returns = analyze_block(if_statement->then_block, true);
      const bool else_returns =
          if_statement->else_block
              ? analyze_block(*if_statement->else_block, true)
              : false;
      return then_returns && if_statement->else_block && else_returns;
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
    } else if (const auto* call =
                   std::get_if<CallExpression>(&expression.data)) {
      state = analyze_call(*call, expression.range);
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
      return ExpressionState{symbol.type, category, *local, {}};
    }

    std::vector<SymbolId> members =
        find_members(current_file_, identifier.name, false);
    if (!members.empty()) {
      const SemanticSymbol& first = model_.symbol(members.front());
      if (first.kind == SymbolKind::kField) {
        return ExpressionState{
            first.type, ValueCategory::kMutableLocation, members.front(), {}};
      }
      return ExpressionState{model_.error_type(),
                             ValueCategory::kCallable,
                             {},
                             std::move(members)};
    }

    if (const std::optional<FileId> file = find_file(identifier.name)) {
      const SemanticSymbol& symbol = model_.symbol(model_.file(*file).symbol);
      if (*file != current_file_ && symbol.visibility == Visibility::kPrivate) {
        diagnostics_.error(range,
                           "file class '" + symbol.name + "' is private");
        return ExpressionState{model_.error_type()};
      }
      return ExpressionState{
          symbol.type, ValueCategory::kType, model_.file(*file).symbol, {}};
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

  ExpressionState analyze_unary(const UnaryExpression& unary,
                                SourceRange range) {
    const ExpressionState operand = analyze_expression(unary.operand);
    if (!check_value(operand, expression_range(unary.operand)) ||
        operand.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    if (unary.operation == TokenKind::kBang) {
      if (operand.type != model_.bool_type()) {
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
    const ExpressionState right = analyze_expression(binary.right);
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
            is_assignable(right.type, left.type)) {
          return ExpressionState{model_.bool_type(), ValueCategory::kValue};
        }
        break;
      case TokenKind::kAmpersandAmpersand:
      case TokenKind::kPipePipe:
        if (left.type == model_.bool_type() &&
            right.type == model_.bool_type()) {
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
    const ExpressionState target = analyze_expression(assignment.target);
    const ExpressionState value = analyze_expression(assignment.value);
    if (target.type != model_.error_type() &&
        target.category != ValueCategory::kMutableLocation) {
      diagnostics_.error(expression_range(assignment.target),
                         "assignment target is not mutable");
    }
    check_value(value, expression_range(assignment.value));
    check_assignment(target.type, value.type, range, "assignment");
    if (target.type == model_.error_type() ||
        value.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    return ExpressionState{
        target.type, ValueCategory::kValue, target.symbol, {}};
  }

  ExpressionState analyze_member_access(const MemberAccessExpression& member,
                                        SourceRange range) {
    const ExpressionState object = analyze_expression(member.object);
    if (object.type == model_.error_type()) {
      return ExpressionState{model_.error_type()};
    }
    const SemanticType& object_type = model_.type(object.type);
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
      if (object.category == ValueCategory::kType) {
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
    std::vector<SymbolId> recovery_matches;
    for (const SymbolId candidate : candidates) {
      const SemanticSymbol& symbol = model_.symbol(candidate);
      if (symbol.parameter_types.size() != arguments.size()) {
        continue;
      }
      bool matches_types = true;
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!is_assignable(symbol.parameter_types[index],
                           arguments[index].type)) {
          matches_types = false;
          break;
        }
      }
      if (matches_types) {
        if (symbol.is_valid) {
          matches.push_back(candidate);
        } else {
          recovery_matches.push_back(candidate);
        }
      }
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
    return ExpressionState{symbol.type, ValueCategory::kValue, selected, {}};
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

  std::optional<FileId> find_file(std::string_view name) const {
    for (std::size_t index = 0; index < files_.size(); ++index) {
      if (files_[index]->name == name &&
          model_.file(FileId{index}).type != model_.error_type()) {
        return FileId{index};
      }
    }
    return std::nullopt;
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
    if (expected == model_.error_type() || actual == model_.error_type() ||
        expected == actual) {
      return true;
    }
    return actual == model_.null_type() && is_reference(expected);
  }

  bool is_reference(TypeId type) const {
    const TypeKind kind = model_.type(type).kind;
    return kind == TypeKind::kString || kind == TypeKind::kFileClass;
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
  std::vector<Scope> scopes_;
  std::optional<std::size_t> current_scope_;
};

SemanticAnalysisResult analyze_semantics(
    std::span<const FileClassDecl* const> files,
    DiagnosticEngine& diagnostics) {
  return SemanticAnalyzer{files, diagnostics}.run();
}

}  // namespace cloth
