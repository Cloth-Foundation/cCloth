// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/hir/hir_verifier.h"

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

namespace cloth {
namespace {

class HirVerifier {
 public:
  HirVerifier(const HirModule& hir, const SemanticModel& semantics,
              DiagnosticEngine& diagnostics)
      : hir_(hir), semantics_(semantics), diagnostics_(diagnostics) {}

  bool run() {
    verify_expressions();
    verify_statements();
    verify_blocks();
    verify_files();
    if (is_valid_) verify_transfer_contexts();
    if (is_valid_ &&
        std::ranges::any_of(semantics_.types(), [](const SemanticType& type) {
          return type.kind == TypeKind::kStruct;
        })) {
      verify_struct_contexts();
    }
    return is_valid_;
  }

 private:
  bool is_struct(TypeId type) const {
    return type.value < semantics_.types().size() &&
           semantics_.type(type).kind == TypeKind::kStruct;
  }

  void verify_value_binding(TypeId expected, TypeId actual, SourceRange range) {
    if ((is_struct(expected) || is_struct(actual)) && expected != actual) {
      report(range, "struct value binding lost nominal identity");
    }
  }

  ValueCategory struct_field_category(const SemanticSymbol& field,
                                      ValueCategory receiver) const {
    if (field.is_final && is_struct(field.type)) {
      return ValueCategory::kReadOnlyLocation;
    }
    if (!field.is_static && field.file &&
        semantics_.file(*field.file).kind == FileTypeKind::kStruct) {
      return receiver == ValueCategory::kMutableLocation ||
                     receiver == ValueCategory::kReadOnlyLocation
                 ? receiver
                 : ValueCategory::kValue;
    }
    return ValueCategory::kMutableLocation;
  }

  const HirExpression& ungroup(HirExpressionId id) const {
    // Malformed cyclic groups must not hang verification.
    for (std::size_t count = 0; count < hir_.storage.expressions().size();
         ++count) {
      const auto* grouped =
          std::get_if<HirGroupedExpression>(&hir_.storage.expression(id).data);
      if (!grouped) break;
      id = grouped->expression;
    }
    return hir_.storage.expression(id);
  }

  void verify_struct_write(HirExpressionId id, FileId file, bool constructor,
                           bool direct_assignment, SourceRange range) {
    const HirExpression& target = ungroup(id);
    std::optional<SymbolId> symbol;
    bool is_self_field = false;
    if (const auto* name = std::get_if<HirSymbolExpression>(&target.data)) {
      symbol = name->symbol;
      is_self_field = semantics_.symbol(*symbol).kind == SymbolKind::kField;
    } else if (const auto* member =
                   std::get_if<HirMemberExpression>(&target.data)) {
      symbol = member->member;
      if (const auto* receiver =
              std::get_if<HirSymbolExpression>(&ungroup(member->object).data)) {
        is_self_field = receiver->symbol == semantics_.file(file).self_symbol;
      }
    }
    const bool initializes_final = symbol && constructor && direct_assignment &&
                                   is_self_field &&
                                   semantics_.symbol(*symbol).file == file &&
                                   semantics_.symbol(*symbol).is_final &&
                                   !semantics_.symbol(*symbol).is_static;
    if (target.category != ValueCategory::kMutableLocation &&
        !initializes_final) {
      report(range, "write does not target a writable storage path");
    }
    if (symbol &&
        (semantics_.symbol(*symbol).kind == SymbolKind::kSelf ||
         (semantics_.symbol(*symbol).is_final && !initializes_final))) {
      report(range, "write replaces self or a final value");
    }
  }

  void verify_struct_expression(const HirExpression& expression, FileId file,
                                bool constructor) {
    const bool struct_owner =
        semantics_.file(file).kind == FileTypeKind::kStruct;
    const ValueCategory receiver = !struct_owner ? ValueCategory::kValue
                                   : constructor
                                       ? ValueCategory::kMutableLocation
                                       : ValueCategory::kReadOnlyLocation;
    std::optional<ValueCategory> expected_category;
    const auto& data = expression.data;
    if (const auto* name = std::get_if<HirSymbolExpression>(&data)) {
      const SemanticSymbol& symbol = semantics_.symbol(name->symbol);
      if (symbol.kind == SymbolKind::kSelf) {
        expected_category = receiver;
        if (name->symbol != semantics_.file(file).self_symbol) {
          report(expression.range, "self belongs to another file");
        }
      } else if (symbol.kind == SymbolKind::kField) {
        expected_category = struct_field_category(symbol, receiver);
      } else if (symbol.kind == SymbolKind::kLocal ||
                 symbol.kind == SymbolKind::kParameter) {
        expected_category = symbol.is_final && is_struct(symbol.type)
                                ? ValueCategory::kReadOnlyLocation
                                : ValueCategory::kMutableLocation;
      }
      if (expected_category)
        verify_value_binding(symbol.type, expression.type, expression.range);
    } else if (const auto* member = std::get_if<HirMemberExpression>(&data)) {
      const HirExpression& object = hir_.storage.expression(member->object);
      if (member->member) {
        const auto& symbol = semantics_.symbol(*member->member);
        if (symbol.kind == SymbolKind::kField) {
          expected_category = struct_field_category(symbol, object.category);
          verify_value_binding(symbol.type, expression.type, expression.range);
          if (is_struct(object.type) &&
              symbol.file != semantics_.type(object.type).file) {
            report(expression.range,
                   "struct field belongs to another nominal type");
          }
        }
      }
    } else if (const auto* grouped = std::get_if<HirGroupedExpression>(&data)) {
      const auto& inner = hir_.storage.expression(grouped->expression);
      expected_category = inner.category;
      verify_value_binding(inner.type, expression.type, expression.range);
    } else if (const auto* index = std::get_if<HirIndexExpression>(&data)) {
      expected_category = ValueCategory::kMutableLocation;
      const auto& array =
          semantics_.type(hir_.storage.expression(index->object).type);
      if (array.element_type)
        verify_value_binding(*array.element_type, expression.type,
                             expression.range);
    } else if (const auto* assignment =
                   std::get_if<HirAssignmentExpression>(&data)) {
      expected_category = ValueCategory::kValue;
      verify_struct_write(assignment->target, file, constructor,
                          assignment->operation == TokenKind::kEqual,
                          expression.range);
      const auto target = hir_.storage.expression(assignment->target).type;
      const auto value = hir_.storage.expression(assignment->value).type;
      verify_value_binding(target, value, expression.range);
      verify_value_binding(target, expression.type, expression.range);
      if (is_struct(target) && assignment->operation != TokenKind::kEqual) {
        report(expression.range,
               "compound assignment cannot operate on a struct");
      }
    } else if (const auto* update = std::get_if<HirUpdateExpression>(&data)) {
      expected_category = ValueCategory::kValue;
      verify_struct_write(update->operand, file, constructor, false,
                          expression.range);
      if (is_struct(hir_.storage.expression(update->operand).type)) {
        report(expression.range, "numeric update cannot operate on a struct");
      }
    } else if (const auto* call = std::get_if<HirCallExpression>(&data)) {
      expected_category = ValueCategory::kValue;
      if (call->callable) {
        const auto& symbol = semantics_.symbol(*call->callable);
        if (call->struct_receiver != struct_receiver_mode(symbol, semantics_) ||
            (call->struct_receiver != StructReceiverMode::kNone &&
             (call->is_base_qualified || call->interface_dispatch ||
              symbol.virtual_slot))) {
          report(expression.range, "struct call has an invalid receiver mode");
        }
        if (call->struct_receiver == StructReceiverMode::kReadOnlyValue &&
            symbol.file) {
          const auto& callee = ungroup(call->callee);
          if (const auto* member =
                  std::get_if<HirMemberExpression>(&callee.data)) {
            if (hir_.storage.expression(member->object).type !=
                semantics_.file(*symbol.file).type) {
              report(expression.range,
                     "struct call has the wrong receiver type");
            }
          } else if (!std::holds_alternative<HirSymbolExpression>(
                         callee.data) ||
                     symbol.file != file) {
            report(expression.range, "struct call has no instance receiver");
          }
        }
        verify_value_binding(symbol.type, expression.type, expression.range);
        if (call->arguments.size() != symbol.parameter_types.size()) {
          report(expression.range,
                 "call argument count does not match declaration");
        } else {
          for (std::size_t index = 0; index < call->arguments.size(); ++index) {
            verify_value_binding(
                symbol.parameter_types[index],
                hir_.storage.expression(call->arguments[index]).type,
                expression.range);
          }
        }
      }
    } else if (const auto* binary = std::get_if<HirBinaryExpression>(&data)) {
      const TypeId left = hir_.storage.expression(binary->left).type;
      const TypeId right = hir_.storage.expression(binary->right).type;
      if ((is_struct(left) || is_struct(right)) &&
          (left != right || expression.type != semantics_.bool_type() ||
           (binary->operation != TokenKind::kEqualEqual &&
            binary->operation != TokenKind::kBangEqual))) {
        report(expression.range,
               "struct binary operation must compare the same nominal type");
      }
    } else if (const auto* array =
                   std::get_if<HirArrayLiteralExpression>(&data)) {
      const auto& type = semantics_.type(expression.type);
      if (type.kind != TypeKind::kArray ||
          type.element_type != array->element_type) {
        report(expression.range, "array literal lost its element type");
      }
      for (const auto element : array->elements) {
        verify_value_binding(array->element_type,
                             hir_.storage.expression(element).type,
                             expression.range);
      }
    }
    if (expected_category && expression.category != *expected_category) {
      report(expression.range, "expression lost its value/storage category");
    }
  }

  // Traverse each declaration with its receiver context. Visited IDs bound work
  // even for malformed cyclic graphs; no source-controlled native recursion.
  void verify_struct_contexts() {
    using Node = std::variant<HirExpressionId, HirStatementId, HirBlockId>;
    for (const HirFileClass& file : hir_.files) {
      const auto check = [&](Node root, const SemanticSymbol* callable) {
        const bool constructor =
            callable && callable->kind == SymbolKind::kConstructor;
        std::vector<Node> pending{root};
        std::unordered_set<std::size_t> seen;
        const auto enqueue = [&](const auto& id) {
          using Id = std::decay_t<decltype(id)>;
          if constexpr (std::is_same_v<Id, HirExpressionId> ||
                        std::is_same_v<Id, HirStatementId> ||
                        std::is_same_v<Id, HirBlockId>) {
            pending.emplace_back(id);
          }
        };
        while (!pending.empty()) {
          const Node next = pending.back();
          pending.pop_back();
          const auto key =
              std::visit([](auto id) { return id.value * 3; }, next) +
              next.index();
          if (!seen.insert(key).second) continue;
          if (const auto* block = std::get_if<HirBlockId>(&next)) {
            for (auto statement : hir_.storage.block(*block).statements)
              enqueue(statement);
            continue;
          }
          const auto children = [&](const auto& node) {
            if constexpr (requires { node.object; }) enqueue(node.object);
            if constexpr (requires { node.operand; }) enqueue(node.operand);
            if constexpr (requires { node.left; }) enqueue(node.left);
            if constexpr (requires { node.right; }) enqueue(node.right);
            if constexpr (requires { node.target; }) enqueue(node.target);
            if constexpr (requires { node.callee; }) enqueue(node.callee);
            if constexpr (requires { node.nullable; }) enqueue(node.nullable);
            if constexpr (requires { node.fallback; }) enqueue(node.fallback);
            if constexpr (requires { node.array; }) enqueue(node.array);
            if constexpr (requires { node.string; }) enqueue(node.string);
            if constexpr (requires { node.expression; })
              enqueue(node.expression);
            if constexpr (requires { node.index; }) enqueue(node.index);
            if constexpr (requires { node.arguments; })
              for (auto id : node.arguments) enqueue(id);
            if constexpr (requires { node.elements; })
              for (auto id : node.elements) enqueue(id);
            if constexpr (requires { node.updates; })
              for (auto id : node.updates) enqueue(id);
            if constexpr (requires { node.body; }) enqueue(node.body);
            if constexpr (requires { node.selector; }) enqueue(node.selector);
            if constexpr (requires { node.arms; })
              for (const auto& arm : node.arms) enqueue(arm.body);
            if constexpr (requires { node.block; }) enqueue(node.block);
            if constexpr (requires { node.iterable; }) enqueue(node.iterable);
            if constexpr (requires { node.then_block; })
              enqueue(node.then_block);
            if constexpr (requires { node.else_block; })
              if (node.else_block) enqueue(*node.else_block);
            if constexpr (requires { node.initializer; })
              if (node.initializer) enqueue(*node.initializer);
            if constexpr (requires { node.condition; }) {
              if constexpr (std::is_same_v<
                                std::decay_t<decltype(node.condition)>,
                                HirExpressionId>) {
                enqueue(node.condition);
              } else if (node.condition)
                enqueue(*node.condition);
            }
            if constexpr (requires { node.value; }) {
              if constexpr (std::is_same_v<std::decay_t<decltype(node.value)>,
                                           HirExpressionId>) {
                enqueue(node.value);
              } else if (node.value)
                enqueue(*node.value);
            }
          };
          if (const auto* id = std::get_if<HirExpressionId>(&next)) {
            const auto& expression = hir_.storage.expression(*id);
            verify_struct_expression(expression, file.file, constructor);
            std::visit(children, expression.data);
          } else {
            const auto& statement =
                hir_.storage.statement(std::get<HirStatementId>(next));
            if (const auto* local =
                    std::get_if<HirLocalStatement>(&statement.data);
                local && local->symbol) {
              const auto type = semantics_.symbol(*local->symbol).type;
              if (local->initializer) {
                verify_value_binding(
                    type, hir_.storage.expression(*local->initializer).type,
                    statement.range);
              } else if (is_struct(type))
                report(statement.range, "struct local has no initializer");
            }
            if (const auto* returned =
                    std::get_if<HirReturnStatement>(&statement.data);
                returned && returned->value && callable && !constructor) {
              verify_value_binding(
                  callable->type,
                  hir_.storage.expression(*returned->value).type,
                  statement.range);
            }
            std::visit(children, statement.data);
          }
        }
      };
      for (const auto& field : file.fields) {
        if (field.initializer) {
          verify_value_binding(semantics_.symbol(field.symbol).type,
                               hir_.storage.expression(*field.initializer).type,
                               semantics_.symbol(field.symbol).range);
          check(*field.initializer, nullptr);
        }
      }
      for (const auto& function : file.functions)
        check(function.body, &semantics_.symbol(function.symbol));
      for (const auto& constructor : file.constructors)
        check(constructor.body, &semantics_.symbol(constructor.symbol));
    }
  }

  void verify_expressions() {
    const auto expressions = hir_.storage.expressions();
    for (const HirExpression& expression : expressions) {
      verify_type(expression.type, expression.range);
      if ((expression.category == ValueCategory::kMutableLocation ||
           expression.category == ValueCategory::kReadOnlyLocation) &&
          !(std::holds_alternative<HirSymbolExpression>(expression.data) ||
            std::holds_alternative<HirMemberExpression>(expression.data) ||
            std::holds_alternative<HirIndexExpression>(expression.data) ||
            std::holds_alternative<HirGroupedExpression>(expression.data))) {
        report(expression.range, "storage category has no location path");
      }
      if (is_struct(expression.type) &&
          !(std::holds_alternative<HirSymbolExpression>(expression.data) ||
            std::holds_alternative<HirTypeExpression>(expression.data) ||
            std::holds_alternative<HirMemberExpression>(expression.data) ||
            std::holds_alternative<HirIndexExpression>(expression.data) ||
            std::holds_alternative<HirGroupedExpression>(expression.data) ||
            std::holds_alternative<HirCallExpression>(expression.data) ||
            std::holds_alternative<HirAssignmentExpression>(expression.data) ||
            std::holds_alternative<HirInvalidExpression>(expression.data))) {
        report(expression.range,
               "struct value has no aggregate representation");
      }
      if (expression.type == semantics_.void_type() &&
          !is_valid_void_expression(expression)) {
        report(expression.range,
               "void expression is not a call or grouped void call");
      }
      if (const auto* literal =
              std::get_if<HirLiteralExpression>(&expression.data)) {
        if ((literal->kind == LiteralKind::kEnum ||
             (expression.type.value < semantics_.types().size() &&
              semantics_.type(expression.type).kind == TypeKind::kEnum)) &&
            (literal->kind != LiteralKind::kEnum ||
             !enum_constant_tag(literal->lexeme, expression.type,
                                semantics_))) {
          report(expression.range, "invalid nominal enum constant");
        }
      } else if (const auto* symbol =
                     std::get_if<HirSymbolExpression>(&expression.data)) {
        verify_symbol(symbol->symbol, expression.range);
        if (symbol->symbol.value < semantics_.symbols().size()) {
          const SemanticSymbol& declaration = semantics_.symbol(symbol->symbol);
          const bool is_storage = declaration.kind == SymbolKind::kField ||
                                  declaration.kind == SymbolKind::kParameter ||
                                  declaration.kind == SymbolKind::kLocal;
          if (declaration.kind == SymbolKind::kEnumCase ||
              (is_storage &&
               (is_enum_type(expression.type) ||
                is_enum_type(declaration.type)) &&
               expression.type != declaration.type)) {
            report(expression.range,
                   "enum symbol does not retain nominal identity");
          }
        }
      } else if (const auto* type =
                     std::get_if<HirTypeExpression>(&expression.data)) {
        verify_type(type->type, expression.range);
      } else if (std::holds_alternative<HirSuperExpression>(expression.data)) {
        if (expression.type == semantics_.error_type() ||
            expression.type.value >= semantics_.types().size() ||
            semantics_.type(expression.type).kind != TypeKind::kFileClass) {
          report(expression.range, "super expression has no base-class type");
        }
      } else if (const auto* unary =
                     std::get_if<HirUnaryExpression>(&expression.data)) {
        verify_expression(unary->operand, expression.range);
        if (const auto type = expression_type(unary->operand);
            type && is_struct(*type)) {
          report(expression.range, "struct used by a unary operation");
        }
        if (is_enum_expression(unary->operand) ||
            is_enum_type(expression.type)) {
          report(expression.range, "enum value used by a unary operation");
        }
      } else if (const auto* update =
                     std::get_if<HirUpdateExpression>(&expression.data)) {
        verify_expression(update->operand, expression.range);
        if (is_enum_expression(update->operand) ||
            is_enum_type(expression.type)) {
          report(expression.range, "enum value used by an update operation");
        }
      } else if (const auto* binary =
                     std::get_if<HirBinaryExpression>(&expression.data)) {
        verify_expression(binary->left, expression.range);
        verify_expression(binary->right, expression.range);
        if (is_enum_type(expression.type) || is_enum_expression(binary->left) ||
            is_enum_expression(binary->right)) {
          if (binary->left.value >= expressions.size() ||
              binary->right.value >= expressions.size() ||
              expressions[binary->left.value].type !=
                  expressions[binary->right.value].type ||
              expression.type != semantics_.bool_type() ||
              (binary->operation != TokenKind::kEqualEqual &&
               binary->operation != TokenKind::kBangEqual)) {
            report(expression.range,
                   "enum binary operation must compare the same nominal type");
          }
        }
      } else if (const auto* test =
                     std::get_if<HirTypeTestExpression>(&expression.data)) {
        verify_expression(test->value, expression.range);
        verify_type(test->target, expression.range);
        if (is_struct(test->target) ||
            (expression_type(test->value) &&
             is_struct(*expression_type(test->value)))) {
          report(expression.range, "struct used by a reference type test");
        }
        if (is_enum_expression(test->value) || is_enum_type(test->target)) {
          report(expression.range, "enum value used by a reference type test");
        }
      } else if (const auto* cast =
                     std::get_if<HirCheckedCastExpression>(&expression.data)) {
        verify_expression(cast->value, expression.range);
        verify_type(cast->target, expression.range);
        if (is_struct(cast->target) ||
            (expression_type(cast->value) &&
             is_struct(*expression_type(cast->value)))) {
          report(expression.range, "struct used by a reference cast");
        }
        if (is_enum_expression(cast->value) || is_enum_type(cast->target)) {
          report(expression.range, "enum value used by a reference cast");
        }
      } else if (const auto* conversion =
                     std::get_if<HirNumericConversionExpression>(
                         &expression.data)) {
        verify_expression(conversion->value, expression.range);
        if (conversion->value.value < expressions.size() &&
            (!is_numeric_type(semantics_.type(expression.type).kind) ||
             !is_numeric_type(
                 semantics_.type(expressions[conversion->value.value].type)
                     .kind))) {
          report(expression.range,
                 "numeric conversion has a non-numeric source or target");
        }
      } else if (const auto* assignment =
                     std::get_if<HirAssignmentExpression>(&expression.data)) {
        verify_expression(assignment->target, expression.range);
        verify_expression(assignment->value, expression.range);
        if (is_enum_type(expression.type) ||
            is_enum_expression(assignment->target) ||
            is_enum_expression(assignment->value)) {
          if (assignment->target.value >= expressions.size() ||
              assignment->value.value >= expressions.size() ||
              expressions[assignment->target.value].type != expression.type ||
              expressions[assignment->value.value].type != expression.type ||
              assignment->operation != TokenKind::kEqual) {
            report(expression.range,
                   "enum assignment must preserve nominal identity");
          }
        }
      } else if (const auto* member =
                     std::get_if<HirMemberExpression>(&expression.data)) {
        verify_expression(member->object, expression.range);
        verify_optional_symbol(member->member, expression.range);
      } else if (const auto* member =
                     std::get_if<HirSafeMemberExpression>(&expression.data)) {
        verify_expression(member->object, expression.range);
        verify_optional_symbol(member->member, expression.range);
      } else if (const auto* coalesce =
                     std::get_if<HirNullCoalesceExpression>(&expression.data)) {
        verify_expression(coalesce->nullable, expression.range);
        verify_expression(coalesce->fallback, expression.range);
      } else if (const auto* assertion =
                     std::get_if<HirNullAssertExpression>(&expression.data)) {
        verify_expression(assertion->operand, expression.range);
      } else if (const auto* call =
                     std::get_if<HirCallExpression>(&expression.data)) {
        verify_expression(call->callee, expression.range);
        verify_optional_symbol(call->callable, expression.range);
        if (call->interface_dispatch) {
          if (!call->callable || call->is_base_qualified ||
              call->interface_dispatch->value >= semantics_.files().size()) {
            report(expression.range,
                   "interface call has incomplete dispatch metadata");
          } else {
            const FileSemantics& interface_file =
                semantics_.file(*call->interface_dispatch);
            if (interface_file.kind != FileTypeKind::kInterface ||
                std::ranges::find(interface_file.interface_functions,
                                  *call->callable) ==
                    interface_file.interface_functions.end()) {
              report(expression.range,
                     "interface call has invalid dispatch metadata");
            }
          }
        }
        if (call->is_base_qualified) {
          if (!call->callable) {
            report(expression.range,
                   "base-qualified call has no callable symbol");
          } else if (call->callable->value < semantics_.symbols().size()) {
            const SemanticSymbol& callable = semantics_.symbol(*call->callable);
            if (callable.kind != SymbolKind::kFunction || callable.is_static ||
                !callable.virtual_slot) {
              report(expression.range,
                     "base-qualified call does not target a virtual instance "
                     "function");
            }
          }
        }
        for (const HirExpressionId argument : call->arguments) {
          verify_expression(argument, expression.range);
        }
      } else if (const auto* array =
                     std::get_if<HirArrayLiteralExpression>(&expression.data)) {
        verify_type(array->element_type, expression.range);
        for (const HirExpressionId element : array->elements) {
          verify_expression(element, expression.range);
        }
      } else if (const auto* index =
                     std::get_if<HirIndexExpression>(&expression.data)) {
        verify_expression(index->object, expression.range);
        verify_expression(index->index, expression.range);
      } else if (const auto* length =
                     std::get_if<HirArrayLengthExpression>(&expression.data)) {
        verify_expression(length->array, expression.range);
      } else if (const auto* meta =
                     std::get_if<HirStringMetaExpression>(&expression.data)) {
        verify_expression(meta->string, expression.range);
      } else if (const auto* meta =
                     std::get_if<HirObjectMetaExpression>(&expression.data)) {
        verify_expression(meta->object, expression.range);
      } else if (const auto* meta =
                     std::get_if<HirIntegerMetaExpression>(&expression.data)) {
        verify_expression(meta->object, expression.range);
        verify_type(meta->operation.integer_type, expression.range);
        verify_integer_meta_object(*meta, expression.range);
      } else if (const auto* call = std::get_if<HirIntegerMetaCallExpression>(
                     &expression.data)) {
        verify_expression(call->object, expression.range);
        verify_type(call->operation.integer_type, expression.range);
        for (const HirExpressionId argument : call->arguments) {
          verify_expression(argument, expression.range);
        }
        verify_integer_meta_call(expression, *call);
      } else if (const auto* grouped =
                     std::get_if<HirGroupedExpression>(&expression.data)) {
        verify_expression(grouped->expression, expression.range);
      }
    }
  }

  bool is_enum_type(TypeId type) const {
    return type.value < semantics_.types().size() &&
           semantics_.type(type).kind == TypeKind::kEnum;
  }

  bool is_enum_expression(HirExpressionId id) const {
    if (id.value >= hir_.storage.expressions().size()) return false;
    const TypeId type = hir_.storage.expression(id).type;
    return is_enum_type(type);
  }

  bool is_valid_void_expression(const HirExpression& expression) const {
    if (const auto* call =
            std::get_if<HirIntegerMetaCallExpression>(&expression.data)) {
      return call->operation.kind == IntegerMetaOperationKind::kWrite;
    }
    if (const auto* call = std::get_if<HirCallExpression>(&expression.data)) {
      return call->callable &&
             call->callable->value < semantics_.symbols().size() &&
             semantics_.symbol(*call->callable).type == semantics_.void_type();
    }
    if (const auto* grouped =
            std::get_if<HirGroupedExpression>(&expression.data)) {
      return grouped->expression.value < hir_.storage.expressions().size() &&
             hir_.storage.expression(grouped->expression).type ==
                 semantics_.void_type();
    }
    return false;
  }

  void verify_switch(const HirSwitchStatement& selection, SourceRange range) {
    verify_expression(selection.selector, range);
    verify_type(selection.selector_type, range);
    if (selection.selector.value >= hir_.storage.expressions().size() ||
        selection.selector_type.value >= semantics_.types().size())
      return;
    const auto& selector = hir_.storage.expression(selection.selector);
    const auto& type = semantics_.type(selection.selector_type);
    const bool is_enum = type.kind == TypeKind::kEnum;
    if (selector.type != selection.selector_type ||
        (!is_enum && !is_integer_type(type.kind)) ||
        (selector.category != ValueCategory::kValue &&
         selector.category != ValueCategory::kMutableLocation &&
         selector.category != ValueCategory::kReadOnlyLocation)) {
      report(range, "switch has an invalid selector type or value category");
      return;
    }
    std::size_t enum_size = 0;
    if (is_enum) {
      if (!type.file || type.file->value >= semantics_.files().size()) {
        report(range, "switch enum has no valid nominal file");
        return;
      }
      enum_size = semantics_.file(*type.file).enum_cases.size();
    }
    if (selection.arms.empty() || selection.arms.size() > kMaxSwitchArms) {
      report(range, "switch has an invalid arm count");
    }
    std::unordered_set<std::uint64_t> values;
    bool has_default = false;
    std::size_t label_count = 0;
    for (std::size_t index = 0; index < selection.arms.size(); ++index) {
      const auto& arm = selection.arms[index];
      verify_block(arm.body, arm.range);
      if (arm.is_default) {
        if (has_default || index + 1 != selection.arms.size() ||
            !arm.labels.empty())
          report(arm.range, "invalid switch default arm");
        has_default = true;
      } else if (arm.labels.empty()) {
        report(arm.range, "switch case arm has no labels");
      }
      if (arm.labels.size() > kMaxSwitchLabels - label_count) {
        report(arm.range, "switch exceeds value label limit");
        return;
      }
      label_count += arm.labels.size();
      for (const auto& label : arm.labels) {
        if (label.value.type != selection.selector_type ||
            (is_enum ? label.value.bits >= enum_size
                     : !is_valid_integer_bits(label.value.bits, type.kind)))
          report(label.range, "switch label has an invalid typed constant");
        if (!values.insert(label.value.bits).second)
          report(label.range, "duplicate normalized switch label");
        if (!label.symbol) continue;
        verify_symbol(*label.symbol, label.range);
        if (label.symbol->value >= semantics_.symbols().size()) continue;
        const auto& symbol = semantics_.symbol(*label.symbol);
        std::optional<ScalarConstant> original;
        if (symbol.kind == SymbolKind::kEnumCase && symbol.enum_tag) {
          original = ScalarConstant{symbol.type, *symbol.enum_tag};
        } else if (symbol.kind == SymbolKind::kField && symbol.is_static &&
                   symbol.is_final) {
          original = symbol.static_constant;
        }
        std::optional<std::uint64_t> expected;
        if (original && original->type.value < semantics_.types().size()) {
          expected = original->type == selection.selector_type
                         ? std::optional{original->bits}
                         : widen_integer_constant(
                               original->bits,
                               semantics_.type(original->type).kind, type.kind);
        }
        if (!symbol.is_valid || !expected || *expected != label.value.bits)
          report(label.range,
                 "switch label disagrees with its constant symbol");
      }
    }
    const bool exhaustive =
        has_default || (is_enum && values.size() == enum_size);
    if (selection.is_exhaustive != exhaustive || (is_enum && !exhaustive))
      report(range, "switch has invalid exhaustiveness metadata");
  }

  // Traverse callable statement trees iteratively. A block cannot be shared
  // between contexts: otherwise a transfer could acquire two different targets.
  void verify_transfer_contexts() {
    struct Work {
      HirBlockId block;
      std::size_t loops;
      std::size_t breaks;
    };
    std::vector<Work> pending;
    for (const auto& file : hir_.files) {
      for (const auto& callable : file.functions)
        pending.push_back({callable.body, 0, 0});
      for (const auto& callable : file.constructors)
        pending.push_back({callable.body, 0, 0});
    }
    std::vector<bool> visited(hir_.storage.blocks().size());
    while (!pending.empty()) {
      const Work work = pending.back();
      pending.pop_back();
      const auto& block = hir_.storage.block(work.block);
      if (visited[work.block.value]) {
        report(block.range, "cyclic or shared callable block");
        continue;
      }
      visited[work.block.value] = true;
      const auto enqueue = [&](HirBlockId child, bool loop,
                               bool selection = false) {
        pending.push_back({child, work.loops + (loop ? 1U : 0U),
                           work.breaks + (loop || selection ? 1U : 0U)});
      };
      for (const auto id : block.statements) {
        const auto& statement = hir_.storage.statement(id);
        const auto& data = statement.data;
        if (std::holds_alternative<HirBreakStatement>(data) && work.breaks == 0)
          report(statement.range, "break has no enclosing loop or switch");
        if (std::holds_alternative<HirContinueStatement>(data) &&
            work.loops == 0)
          report(statement.range, "continue has no enclosing loop");
        if (const auto* node = std::get_if<HirIfStatement>(&data)) {
          enqueue(node->then_block, false);
          if (node->else_block) enqueue(*node->else_block, false);
        } else if (const auto* node = std::get_if<HirWhileStatement>(&data)) {
          enqueue(node->body, true);
        } else if (const auto* node = std::get_if<HirForEachStatement>(&data)) {
          enqueue(node->body, true);
        } else if (const auto* node = std::get_if<HirForStatement>(&data)) {
          enqueue(node->body, true);
        } else if (const auto* node = std::get_if<HirSwitchStatement>(&data)) {
          for (const auto& arm : node->arms) enqueue(arm.body, false, true);
        } else if (const auto* node =
                       std::get_if<HirNestedBlockStatement>(&data)) {
          enqueue(node->block, false);
        }
      }
    }
  }

  void verify_statements() {
    for (const HirStatement& statement : hir_.storage.statements()) {
      if (const auto* local = std::get_if<HirLocalStatement>(&statement.data)) {
        verify_optional_symbol(local->symbol, statement.range);
        if (local->initializer) {
          verify_expression(*local->initializer, statement.range);
        }
      } else if (const auto* return_statement =
                     std::get_if<HirReturnStatement>(&statement.data)) {
        if (return_statement->value) {
          verify_expression(*return_statement->value, statement.range);
        }
      } else if (const auto* expression =
                     std::get_if<HirExpressionStatement>(&statement.data)) {
        verify_expression(expression->expression, statement.range);
      } else if (const auto* if_statement =
                     std::get_if<HirIfStatement>(&statement.data)) {
        verify_expression(if_statement->condition, statement.range);
        verify_block(if_statement->then_block, statement.range);
        if (if_statement->else_block) {
          verify_block(*if_statement->else_block, statement.range);
        }
      } else if (const auto* while_statement =
                     std::get_if<HirWhileStatement>(&statement.data)) {
        verify_expression(while_statement->condition, statement.range);
        verify_block(while_statement->body, statement.range);
      } else if (const auto* for_statement =
                     std::get_if<HirForEachStatement>(&statement.data)) {
        verify_optional_symbol(for_statement->variable, statement.range);
        verify_expression(for_statement->iterable, statement.range);
        verify_block(for_statement->body, statement.range);
      } else if (const auto* for_statement =
                     std::get_if<HirForStatement>(&statement.data)) {
        if (for_statement->initializer &&
            for_statement->initializer->value >=
                hir_.storage.statements().size()) {
          report(statement.range,
                 "for initializer references an unknown statement");
        } else if (for_statement->initializer) {
          const HirStatementData& initializer =
              hir_.storage.statement(*for_statement->initializer).data;
          if (!std::holds_alternative<HirLocalStatement>(initializer) &&
              !std::holds_alternative<HirExpressionStatement>(initializer)) {
            report(statement.range,
                   "for initializer is not a local or expression statement");
          }
        }
        if (for_statement->condition) {
          verify_expression(*for_statement->condition, statement.range);
        }
        for (const HirExpressionId update : for_statement->updates) {
          verify_expression(update, statement.range);
        }
        verify_block(for_statement->body, statement.range);
      } else if (const auto* selection =
                     std::get_if<HirSwitchStatement>(&statement.data)) {
        verify_switch(*selection, statement.range);
      } else if (const auto* nested =
                     std::get_if<HirNestedBlockStatement>(&statement.data)) {
        verify_block(nested->block, statement.range);
      }
    }
  }

  void verify_blocks() {
    for (const HirBlock& block : hir_.storage.blocks()) {
      for (const HirStatementId statement : block.statements) {
        if (statement.value >= hir_.storage.statements().size()) {
          report(block.range, "block references an unknown statement");
        }
      }
    }
  }

  void verify_files() {
    if (hir_.files.size() > semantics_.files().size()) {
      report(fallback_range(),
             "file count exceeds the semantic declaration model");
    }
    for (std::size_t file_index = 0; file_index < hir_.files.size();
         ++file_index) {
      const HirFileClass& file = hir_.files[file_index];
      const SourceRange range = symbol_range(file.symbol);
      if (file.file.value >= semantics_.files().size()) {
        report(range, "file class has an unknown FileId");
      } else {
        const FileSemantics& semantic_file = semantics_.file(file.file);
        if (file.file != FileId{file_index}) {
          report(range, "file class order does not match its FileId");
        }
        if (file.symbol != semantic_file.symbol) {
          report(range, "file class symbol does not match semantics");
        }
        if (file.base_file != semantic_file.base_file) {
          report(range, "file class base does not match semantics");
        }
        if (file.base_file &&
            (file.base_file->value >= semantics_.files().size() ||
             *file.base_file == file.file)) {
          report(range, "file class has an invalid base FileId");
        }
        if (file.fields.size() != semantic_file.fields.size() ||
            file.functions.size() != semantic_file.functions.size() ||
            file.constructors.size() != semantic_file.constructors.size()) {
          report(range, "file member counts do not match semantics");
        }
      }
      verify_symbol(file.symbol, range);
      for (const HirField& field : file.fields) {
        verify_symbol(field.symbol, range);
        if (field.initializer) {
          verify_expression(*field.initializer, range);
        }
      }
      for (const HirCallable& function : file.functions) {
        verify_callable(function, range);
      }
      for (const HirCallable& constructor : file.constructors) {
        verify_callable(constructor, range);
      }
      for (const MemberReference& member : file.member_order) {
        switch (member.kind) {
          case DeclarationKind::kField:
            if (member.index >= file.fields.size()) {
              report(range, "member order references an unknown field");
            }
            break;
          case DeclarationKind::kFunction:
            if (member.index >= file.functions.size()) {
              report(range, "member order references an unknown function");
            }
            break;
          case DeclarationKind::kConstructor:
            if (member.index >= file.constructors.size()) {
              report(range, "member order references an unknown constructor");
            }
            break;
          case DeclarationKind::kNestedType:
            break;
        }
      }
    }
  }

  void verify_callable(const HirCallable& callable, SourceRange range) {
    verify_symbol(callable.symbol, range);
    if (callable.symbol.value < semantics_.symbols().size() &&
        callable.struct_receiver !=
            struct_receiver_mode(semantics_.symbol(callable.symbol),
                                 semantics_)) {
      report(range, "callable has an invalid struct receiver mode");
    }
    if (callable.initializer) {
      verify_symbol(callable.initializer->constructor, range);
      for (const HirExpressionId argument : callable.initializer->arguments) {
        verify_expression(argument, range);
      }
      if (callable.symbol.value < semantics_.symbols().size() &&
          semantics_.symbol(callable.symbol).base_constructor !=
              callable.initializer->constructor) {
        report(range,
               "constructor initializer does not match semantic binding");
      }
    } else if (callable.symbol.value < semantics_.symbols().size() &&
               semantics_.symbol(callable.symbol).base_constructor) {
      report(range, "constructor lost its semantic base initializer");
    }
    verify_block(callable.body, range);
  }

  void verify_integer_meta_object(const HirIntegerMetaExpression& meta,
                                  SourceRange range) {
    const std::optional<TypeId> object = expression_type(meta.object);
    if (!object || object->value >= semantics_.types().size() ||
        meta.operation.integer_type.value >= semantics_.types().size()) {
      return;
    }
    if (meta.operation.kind == IntegerMetaOperationKind::kWrite) {
      if (*object != meta.operation.integer_type ||
          !is_integer_type(semantics_.type(*object).kind)) {
        report(range, "integer write meta operation has the wrong receiver");
      }
    } else if (!is_byte_array(*object)) {
      report(range, "integer read meta operation has the wrong receiver");
    }
  }

  void verify_integer_meta_call(const HirExpression& expression,
                                const HirIntegerMetaCallExpression& call) {
    const std::optional<TypeId> object = expression_type(call.object);
    if (!object || object->value >= semantics_.types().size() ||
        call.operation.integer_type.value >= semantics_.types().size()) {
      return;
    }
    if (call.operation.kind == IntegerMetaOperationKind::kWrite) {
      if (expression.type != semantics_.void_type() ||
          *object != call.operation.integer_type ||
          !is_integer_type(semantics_.type(*object).kind) ||
          call.arguments.size() != 2) {
        report(expression.range,
               "integer endian write has incompatible HIR metadata");
        return;
      }
      const std::optional<TypeId> destination =
          expression_type(call.arguments[0]);
      const std::optional<TypeId> offset = expression_type(call.arguments[1]);
      if (!destination || !is_byte_array(*destination) || !offset ||
          !is_int32_compatible(*offset)) {
        report(expression.range,
               "integer endian write has incompatible arguments");
      }
      return;
    }
    if (expression.type.value >= semantics_.types().size() ||
        expression.type != call.operation.integer_type ||
        !is_integer_type(semantics_.type(expression.type).kind) ||
        !is_byte_array(*object) || call.arguments.size() != 1) {
      report(expression.range,
             "integer endian read has incompatible HIR metadata");
      return;
    }
    const std::optional<TypeId> offset = expression_type(call.arguments[0]);
    if (!offset || !is_int32_compatible(*offset)) {
      report(expression.range, "integer endian read has incompatible offset");
    }
  }

  std::optional<TypeId> expression_type(HirExpressionId expression) const {
    if (expression.value >= hir_.storage.expressions().size()) {
      return std::nullopt;
    }
    return hir_.storage.expression(expression).type;
  }

  bool is_byte_array(TypeId type) const {
    if (type.value >= semantics_.types().size()) {
      return false;
    }
    const SemanticType& array = semantics_.type(type);
    return array.kind == TypeKind::kArray &&
           array.element_type == semantics_.find_type("byte");
  }

  bool is_int32_compatible(TypeId type) const {
    if (type.value >= semantics_.types().size()) {
      return false;
    }
    const TypeId int32_type = *semantics_.find_type("int32");
    return type == int32_type ||
           can_widen_numeric(semantics_.type(type).kind,
                             semantics_.type(int32_type).kind);
  }

  void verify_type(TypeId type, SourceRange range) {
    if (type.value >= semantics_.types().size()) {
      report(range, "expression references an unknown type");
    }
  }

  void verify_symbol(SymbolId symbol, SourceRange range) {
    if (symbol.value >= semantics_.symbols().size()) {
      report(range, "node references an unknown symbol");
    }
  }

  void verify_optional_symbol(std::optional<SymbolId> symbol,
                              SourceRange range) {
    if (symbol) {
      verify_symbol(*symbol, range);
    }
  }

  void verify_expression(HirExpressionId expression, SourceRange range) {
    if (expression.value >= hir_.storage.expressions().size()) {
      report(range, "node references an unknown expression");
    }
  }

  void verify_block(HirBlockId block, SourceRange range) {
    if (block.value >= hir_.storage.blocks().size()) {
      report(range, "node references an unknown block");
    }
  }

  SourceRange symbol_range(SymbolId symbol) const {
    if (symbol.value < semantics_.symbols().size()) {
      return semantics_.symbol(symbol).range;
    }
    return fallback_range();
  }

  static SourceRange fallback_range() noexcept {
    return point_range(SourceLocation{"<hir>", 0, 1, 1});
  }

  void report(SourceRange range, std::string message) {
    diagnostics_.error(range, "internal HIR verification error: " + message);
    is_valid_ = false;
  }

  const HirModule& hir_;
  const SemanticModel& semantics_;
  DiagnosticEngine& diagnostics_;
  bool is_valid_{true};
};

}  // namespace

bool verify_hir(const HirModule& hir, const SemanticModel& semantics,
                DiagnosticEngine& diagnostics) {
  return HirVerifier{hir, semantics, diagnostics}.run();
}

}  // namespace cloth
