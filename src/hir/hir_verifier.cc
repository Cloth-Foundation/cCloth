// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/hir/hir_verifier.h"

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/lexer/literal.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/scalar_constants.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <algorithm>
#include <cstddef>
#include <map>
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
    if (!verify_constant_limits()) return false;
    verify_expressions();
    verify_statements();
    verify_blocks();
    verify_files();
    if (is_valid_) verify_constants();
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
  bool verify_constant_limits() {
    std::map<std::string, std::pair<std::size_t, std::size_t>> budgets;
    for (const auto& file : hir_.files) {
      if (file.file.value >= semantics_.files().size()) continue;
      auto& budget = budgets[semantics_.file(file.file).identity.package.name];
      for (const auto& field : file.fields) {
        if (field.symbol.value >= semantics_.symbols().size() ||
            !semantics_.symbol(field.symbol).is_static)
          continue;
        if (++budget.first > kMaxStaticConstants) {
          report(symbol_range(field.symbol),
                 "package exceeds 65536 static constants");
          return false;
        }
        if (!field.initializer) continue;
        std::vector<std::pair<HirExpressionId, std::size_t>> work{
            {*field.initializer, 1}};
        std::size_t count = 0;
        // Bound retained initializer trees before recursive verification.
        // Source budgets are additionally enforced before HIR lowering.
        while (!work.empty()) {
          const auto [id, depth] = work.back();
          work.pop_back();
          if (id.value >= hir_.storage.expressions().size()) {
            report(symbol_range(field.symbol),
                   "constant references an unknown expression");
            return false;
          }
          const auto& expression = hir_.storage.expression(id);
          if (++count > kMaxConstantNodes ||
              ++budget.second > kMaxPackageConstantNodes ||
              depth > kMaxConstantDepth) {
            report(expression.range,
                   "constant expression exceeds node or nesting limits");
            return false;
          }
          const auto push = [&](HirExpressionId child) {
            work.emplace_back(child, depth + 1);
          };
          const auto& data = expression.data;
          if (const auto* literal = std::get_if<HirLiteralExpression>(&data)) {
            if ((literal->kind == LiteralKind::kInteger ||
                 literal->kind == LiteralKind::kFloat) &&
                literal->lexeme.size() > kMaxConstantLiteralBytes) {
              report(expression.range,
                     "constant numeric literal exceeds 4096 bytes");
              return false;
            }
          } else if (const auto* unary =
                         std::get_if<HirUnaryExpression>(&data)) {
            push(unary->operand);
          } else if (const auto* binary =
                         std::get_if<HirBinaryExpression>(&data)) {
            push(binary->right);
            push(binary->left);
          } else if (const auto* conversion =
                         std::get_if<HirNumericConversionExpression>(&data)) {
            push(conversion->value);
          } else if (const auto* conversion =
                         std::get_if<HirIntegerConversionExpression>(&data)) {
            push(conversion->value);
          } else if (const auto* group =
                         std::get_if<HirGroupedExpression>(&data)) {
            push(group->expression);
          } else if (const auto* member =
                         std::get_if<HirMemberExpression>(&data)) {
            push(member->object);
          } else if (!std::holds_alternative<HirSymbolExpression>(data) &&
                     !std::holds_alternative<HirTypeExpression>(data)) {
            report(expression.range, "ineligible static constant expression");
            return false;
          }
        }
      }
    }
    return true;
  }

  // Evaluate each bounded expression using dependency claims, then compare the
  // result to both HIR and semantics. An independent graph walk rejects cycles,
  // including references in short-circuited branches.
  void verify_constants() {
    std::map<std::size_t, std::vector<std::size_t>> edges;
    std::map<std::string, std::size_t> counts;
    for (const auto& symbol : semantics_.symbols()) {
      if (symbol.kind == SymbolKind::kField && symbol.is_static) {
        if (symbol.file && symbol.file->value < semantics_.files().size() &&
            ++counts[semantics_.file(*symbol.file).identity.package.name] >
                kMaxStaticConstants) {
          report(symbol.range, "package exceeds 65536 static constants");
          return;
        }
        if (!symbol.is_final || !symbol.static_constant ||
            !is_valid_scalar_constant(*symbol.static_constant, symbol.type,
                                      semantics_))
          report(symbol.range, "static field has no valid scalar constant");
      } else if (symbol.static_constant) {
        report(symbol.range, "non-static field carries a scalar constant");
      }
    }
    for (const auto& file : hir_.files) {
      for (const auto& field : file.fields) {
        const auto& symbol = semantics_.symbol(field.symbol);
        if (symbol.kind != SymbolKind::kField || symbol.file != file.file)
          report(symbol.range, "field ownership does not match semantics");
        if (!symbol.is_static) {
          if (field.static_constant)
            report(symbol.range, "instance field carries a static constant");
          continue;
        }
        if (!field.initializer || !field.static_constant ||
            !is_valid_scalar_constant(*field.static_constant, symbol.type,
                                      semantics_) ||
            field.static_constant != symbol.static_constant) {
          report(symbol.range,
                 "static field constant does not match semantics");
          continue;
        }
        auto& dependencies = edges[field.symbol.value];
        std::vector<HirExpressionId> work{*field.initializer};
        while (!work.empty()) {
          const auto id = work.back();
          work.pop_back();
          const auto& expression = hir_.storage.expression(id);
          const auto& data = expression.data;
          std::optional<SymbolId> reference;
          if (const auto literal = constant_literal(id)) {
            if (!scalar_literal(literal->literal->kind,
                                literal->literal->lexeme,
                                semantics_.type(expression.type).kind,
                                !literal->signs.empty() &&
                                    literal->signs.back() == TokenKind::kMinus))
              report(expression.range,
                     "constant literal does not fit its type");
            continue;
          }
          if (const auto* name = std::get_if<HirSymbolExpression>(&data)) {
            reference = name->symbol;
          } else if (const auto* member =
                         std::get_if<HirMemberExpression>(&data)) {
            reference = member->member;
            const auto& object = ungroup(member->object);
            const auto* qualifier =
                std::get_if<HirTypeExpression>(&object.data);
            auto owner = qualifier ? semantics_.type(qualifier->type).file
                                   : std::nullopt;
            bool matches = false;
            for (std::size_t depth = 0;
                 reference && owner &&
                 owner->value < semantics_.files().size() &&
                 depth < semantics_.files().size();
                 ++depth) {
              if (owner == semantics_.symbol(*reference).file) {
                matches = true;
                break;
              }
              owner = semantics_.file(*owner).base_file;
            }
            if (!matches)
              report(expression.range,
                     "constant member has an invalid type qualifier");
          } else if (const auto* group =
                         std::get_if<HirGroupedExpression>(&data)) {
            work.push_back(group->expression);
            if (expression.type !=
                hir_.storage.expression(group->expression).type)
              report(expression.range,
                     "constant group has an inconsistent type");
          } else if (const auto* unary =
                         std::get_if<HirUnaryExpression>(&data)) {
            work.push_back(unary->operand);
            if (expression.type !=
                    hir_.storage.expression(unary->operand).type ||
                !unary_scalar(unary->operation,
                              semantics_.type(expression.type).kind, 0))
              report(expression.range,
                     "constant unary operation has inconsistent types");
            if (unary->operand_is_presence_test)
              report(expression.range,
                     "constant unary operation has a presence test");
          } else if (const auto* binary =
                         std::get_if<HirBinaryExpression>(&data)) {
            work.push_back(binary->left);
            work.push_back(binary->right);
            const auto left = hir_.storage.expression(binary->left).type;
            const auto right = hir_.storage.expression(binary->right).type;
            const auto a = semantics_.type(left).kind;
            const auto b = semantics_.type(right).kind;
            const bool shift = binary->operation == TokenKind::kShiftLeft ||
                               binary->operation == TokenKind::kShiftRight;
            const auto common = !shift && can_widen_numeric(a, b) ? b : a;
            bool boolean = false;
            switch (binary->operation) {
              case TokenKind::kEqualEqual:
              case TokenKind::kBangEqual:
              case TokenKind::kLess:
              case TokenKind::kLessEqual:
              case TokenKind::kGreater:
              case TokenKind::kGreaterEqual:
              case TokenKind::kAmpersandAmpersand:
              case TokenKind::kPipePipe:
                boolean = true;
                break;
              default:
                break;
            }
            const auto one = common == TypeKind::kFloat32 ? 0x3f800000ULL
                             : common == TypeKind::kFloat64
                                 ? 0x3ff0000000000000ULL
                                 : 1ULL;
            if ((!shift && left != right && !can_widen_numeric(a, b) &&
                 !can_widen_numeric(b, a)) ||
                semantics_.type(expression.type).kind !=
                    (boolean ? TypeKind::kBool : common) ||
                !binary_scalar(binary->operation, common, one, one,
                               shift ? b : common))
              report(expression.range,
                     "constant binary operation has inconsistent types");
            if (binary->left_is_presence_test || binary->right_is_presence_test)
              report(expression.range,
                     "constant binary operation has a presence test");
          } else if (const auto* conversion =
                         std::get_if<HirNumericConversionExpression>(&data)) {
            if (const auto literal = constant_literal(conversion->value)) {
              if (!scalar_literal(
                      literal->literal->kind, literal->literal->lexeme,
                      semantics_.type(expression.type).kind,
                      !literal->signs.empty() &&
                          literal->signs.back() == TokenKind::kMinus))
                report(expression.range,
                       "constant literal conversion is out of range");
            } else {
              work.push_back(conversion->value);
            }
          } else if (const auto* conversion =
                         std::get_if<HirIntegerConversionExpression>(&data)) {
            work.push_back(conversion->value);
            const HirExpression& operand =
                hir_.storage.expression(conversion->value);
            const bool valid_mode =
                conversion->mode == IntegerConversionMode::kWrap ||
                conversion->mode == IntegerConversionMode::kSat;
            if (!valid_mode ||
                !is_integer_type(semantics_.type(expression.type).kind) ||
                !is_integer_type(semantics_.type(operand.type).kind) ||
                expression.category != ValueCategory::kValue) {
              report(expression.range,
                     "integer conversion has inconsistent constant metadata");
            }
          } else if (const auto* literal =
                         std::get_if<HirLiteralExpression>(&data)) {
            if (literal->kind != LiteralKind::kEnum &&
                !scalar_literal(literal->kind, literal->lexeme,
                                semantics_.type(expression.type).kind))
              report(expression.range,
                     "constant literal has an invalid kind or value");
          } else {
            report(expression.range, "ineligible static constant expression");
          }
          if (!is_scalar_constant_type(semantics_.type(expression.type).kind))
            report(expression.range,
                   "constant expression has a non-scalar type");
          if (reference) {
            const auto& target = semantics_.symbol(*reference);
            if (target.kind != SymbolKind::kField || !target.is_static ||
                !target.is_final || target.type != expression.type)
              report(expression.range,
                     "constant reference has an invalid binding");
            dependencies.push_back(reference->value);
          }
        }
      }
    }
    if (!is_valid_) return;
    std::map<std::size_t, unsigned> state;
    for (const auto& [root, unused] : edges) {
      if (state[root] == 2) continue;
      std::vector<std::pair<std::size_t, std::size_t>> stack{{root, 0}};
      state[root] = 1;
      while (!stack.empty()) {
        auto& [node, next] = stack.back();
        if (next < edges[node].size()) {
          const auto dependency = edges[node][next++];
          if (state[dependency] == 1) {
            report(semantics_.symbol(SymbolId{node}).range,
                   "cyclic HIR static constant dependency");
            return;
          }
          if (state[dependency] == 0) {
            state[dependency] = 1;
            stack.emplace_back(dependency, 0);
          }
        } else {
          state[node] = 2;
          stack.pop_back();
        }
      }
    }
    for (const auto& file : hir_.files) {
      for (const auto& field : file.fields) {
        if (!field.static_constant) continue;
        const auto value = constant_expression(*field.initializer);
        const auto type = field.static_constant->type;
        const auto bits =
            value
                ? convert_scalar(value->bits, semantics_.type(value->type).kind,
                                 semantics_.type(type).kind)
                : ConstantBits{
                      std::unexpected(ConstantError::kInvalidOperation)};
        if (!value ||
            (value->type != type &&
             !can_widen_numeric(semantics_.type(value->type).kind,
                                semantics_.type(type).kind)) ||
            !bits || *bits != field.static_constant->bits)
          report(symbol_range(field.symbol),
                 "static constant claim disagrees with its initializer");
      }
    }
  }

  struct SignedLiteral {
    const HirLiteralExpression* literal;
    std::vector<TokenKind> signs;
  };

  std::optional<SignedLiteral> constant_literal(HirExpressionId id) const {
    std::vector<TokenKind> signs;
    for (;;) {
      const auto& expression = hir_.storage.expression(id);
      const auto& data = expression.data;
      if (const auto* group = std::get_if<HirGroupedExpression>(&data)) {
        if (expression.type != hir_.storage.expression(group->expression).type)
          return std::nullopt;
        id = group->expression;
      } else if (const auto* unary = std::get_if<HirUnaryExpression>(&data)) {
        if (unary->operand_is_presence_test ||
            expression.type != hir_.storage.expression(unary->operand).type)
          return std::nullopt;
        if (unary->operation != TokenKind::kPlus &&
            unary->operation != TokenKind::kMinus)
          return std::nullopt;
        signs.push_back(unary->operation);
        id = unary->operand;
      } else if (const auto* literal = std::get_if<HirLiteralExpression>(&data);
                 literal && (literal->kind == LiteralKind::kInteger ||
                             literal->kind == LiteralKind::kFloat)) {
        return SignedLiteral{literal, std::move(signs)};
      } else {
        return std::nullopt;
      }
    }
  }

  std::optional<ScalarConstant> constant_expression(HirExpressionId id) const {
    const auto& expression = hir_.storage.expression(id);
    const auto kind = semantics_.type(expression.type).kind;
    const auto result =
        [&](ConstantBits bits) -> std::optional<ScalarConstant> {
      if (!bits) return std::nullopt;
      return ScalarConstant{expression.type, *bits};
    };
    if (const auto literal = constant_literal(id))
      return result(scalar_signed_literal(literal->literal->kind,
                                          literal->literal->lexeme, kind,
                                          literal->signs));
    const auto& data = expression.data;
    if (const auto* literal = std::get_if<HirLiteralExpression>(&data)) {
      if (literal->kind == LiteralKind::kEnum) {
        const auto tag =
            enum_constant_tag(literal->lexeme, expression.type, semantics_);
        return tag ? result(*tag) : std::nullopt;
      }
      return result(scalar_literal(literal->kind, literal->lexeme, kind));
    }
    if (const auto* group = std::get_if<HirGroupedExpression>(&data))
      return constant_expression(group->expression);
    if (const auto* name = std::get_if<HirSymbolExpression>(&data))
      return semantics_.symbol(name->symbol).static_constant;
    if (const auto* member = std::get_if<HirMemberExpression>(&data))
      return member->member ? semantics_.symbol(*member->member).static_constant
                            : std::nullopt;
    if (const auto* conversion =
            std::get_if<HirNumericConversionExpression>(&data)) {
      if (const auto literal = constant_literal(conversion->value))
        return result(scalar_signed_literal(literal->literal->kind,
                                            literal->literal->lexeme, kind,
                                            literal->signs));
      const auto operand = constant_expression(conversion->value);
      return operand ? result(convert_scalar(
                           operand->bits, semantics_.type(operand->type).kind,
                           kind))
                     : std::nullopt;
    }
    if (const auto* conversion =
            std::get_if<HirIntegerConversionExpression>(&data)) {
      const auto operand = constant_expression(conversion->value);
      return operand ? result(convert_integer_mode(
                           operand->bits, semantics_.type(operand->type).kind,
                           kind, conversion->mode))
                     : std::nullopt;
    }
    if (const auto* unary = std::get_if<HirUnaryExpression>(&data)) {
      const auto operand = constant_expression(unary->operand);
      if (!operand || operand->type != expression.type) return std::nullopt;
      return result(unary_scalar(unary->operation, kind, operand->bits));
    }
    const auto* binary = std::get_if<HirBinaryExpression>(&data);
    if (!binary) return std::nullopt;
    const auto left = constant_expression(binary->left);
    if (!left) return std::nullopt;
    if ((binary->operation == TokenKind::kAmpersandAmpersand && !left->bits) ||
        (binary->operation == TokenKind::kPipePipe && left->bits))
      return kind == TypeKind::kBool ? result(left->bits != 0) : std::nullopt;
    const auto right = constant_expression(binary->right);
    if (!right) return std::nullopt;
    const auto a = semantics_.type(left->type).kind;
    const auto b = semantics_.type(right->type).kind;
    if (binary->operation == TokenKind::kShiftLeft ||
        binary->operation == TokenKind::kShiftRight)
      return left->type == expression.type
                 ? result(binary_scalar(binary->operation, a, left->bits,
                                        right->bits, b))
                 : std::nullopt;
    if (left->type != right->type && !can_widen_numeric(a, b) &&
        !can_widen_numeric(b, a))
      return std::nullopt;
    const auto common = can_widen_numeric(a, b) ? b : a;
    const auto l = convert_scalar(left->bits, a, common);
    const auto r = convert_scalar(right->bits, b, common);
    if (!l || !r) return std::nullopt;
    return result(binary_scalar(binary->operation, common, *l, *r, common));
  }

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
    struct TransparentParent {
      std::optional<std::size_t> expression;
      bool flips_sign{false};
      bool is_ambiguous{false};
    };
    std::vector<TransparentParent> transparent_parents(expressions.size());
    const auto record_parent = [&](HirExpressionId child, std::size_t parent,
                                   bool flips_sign) {
      if (child.value >= transparent_parents.size()) {
        return;
      }
      TransparentParent& edge = transparent_parents[child.value];
      if (edge.expression) {
        edge.is_ambiguous = true;
        return;
      }
      edge.expression = parent;
      edge.flips_sign = flips_sign;
    };
    for (std::size_t index = 0; index < expressions.size(); ++index) {
      if (const auto* unary =
              std::get_if<HirUnaryExpression>(&expressions[index].data);
          unary != nullptr && (unary->operation == TokenKind::kPlus ||
                               unary->operation == TokenKind::kMinus)) {
        record_parent(unary->operand, index,
                      unary->operation == TokenKind::kMinus);
      } else if (const auto* grouped = std::get_if<HirGroupedExpression>(
                     &expressions[index].data)) {
        record_parent(grouped->expression, index, false);
      }
    }

    for (std::size_t index = 0; index < expressions.size(); ++index) {
      const HirExpression& expression = expressions[index];
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
        if (literal->kind == LiteralKind::kInteger ||
            literal->kind == LiteralKind::kFloat) {
          const NumericLiteralSpelling spelling =
              parse_numeric_literal_spelling(literal->lexeme);
          const bool has_suffix =
              spelling.suffix_kind != NumericLiteralSuffix::kNone;
          bool valid_type = false;
          bool valid_value = false;
          if (expression.type.value < semantics_.types().size()) {
            const TypeKind type = semantics_.type(expression.type).kind;
            valid_type = is_numeric_type(type);
            if (valid_type &&
                spelling.error == NumericLiteralSpellingError::kNone &&
                !has_suffix) {
              valid_value = scalar_literal(literal->kind, spelling.core, type)
                                .has_value();
              if (!valid_value && literal->kind == LiteralKind::kInteger) {
                bool is_negated = false;
                bool valid_chain = true;
                std::size_t current = index;
                std::size_t depth = 0;
                while (transparent_parents[current].expression &&
                       depth++ < expressions.size()) {
                  const TransparentParent& edge = transparent_parents[current];
                  if (edge.is_ambiguous) {
                    valid_chain = false;
                    break;
                  }
                  is_negated = is_negated != edge.flips_sign;
                  current = *edge.expression;
                }
                if (depth > expressions.size()) {
                  valid_chain = false;
                }
                // A signed minimum retains its unsigned magnitude beneath the
                // unary minus expression that gives it its final value.
                valid_value =
                    valid_chain && is_negated &&
                    scalar_literal(literal->kind, spelling.core, type, true)
                        .has_value();
              }
            }
          }
          if (spelling.error != NumericLiteralSpellingError::kNone ||
              has_suffix || !valid_type || !valid_value ||
              (literal->kind == LiteralKind::kInteger &&
               spelling.core_is_floating)) {
            report(expression.range,
                   "numeric literal has invalid canonical spelling or type");
          }
        }
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
        if (unary->operation == TokenKind::kPlus ||
            unary->operation == TokenKind::kMinus) {
          const std::optional<TypeId> operand = expression_type(unary->operand);
          const bool valid = operand &&
                             operand->value < semantics_.types().size() &&
                             expression.type == *operand &&
                             is_numeric_type(semantics_.type(*operand).kind) &&
                             !unary->operand_is_presence_test;
          if (!valid) {
            report(expression.range,
                   "numeric unary expression has incompatible HIR metadata");
          }
        }
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
        const std::optional<TypeId> operand = expression_type(update->operand);
        const bool valid = operand &&
                           operand->value < semantics_.types().size() &&
                           expression.type == *operand &&
                           is_numeric_type(semantics_.type(*operand).kind) &&
                           (update->operation == TokenKind::kPlusPlus ||
                            update->operation == TokenKind::kMinusMinus) &&
                           hir_.storage.expression(update->operand).category ==
                               ValueCategory::kMutableLocation &&
                           expression.category == ValueCategory::kValue;
        if (!valid) {
          report(expression.range,
                 "numeric update has incompatible HIR metadata");
        }
        if (is_enum_expression(update->operand) ||
            is_enum_type(expression.type)) {
          report(expression.range, "enum value used by an update operation");
        }
      } else if (const auto* binary =
                     std::get_if<HirBinaryExpression>(&expression.data)) {
        verify_expression(binary->left, expression.range);
        verify_expression(binary->right, expression.range);
        const bool arithmetic = binary->operation == TokenKind::kPlus ||
                                binary->operation == TokenKind::kMinus ||
                                binary->operation == TokenKind::kStar ||
                                binary->operation == TokenKind::kSlash ||
                                binary->operation == TokenKind::kPercent;
        if (arithmetic) {
          const std::optional<TypeId> left = expression_type(binary->left);
          const std::optional<TypeId> right = expression_type(binary->right);
          bool numeric = false;
          bool concatenate = false;
          if (left && right && left->value < semantics_.types().size() &&
              right->value < semantics_.types().size()) {
            const TypeKind left_kind = semantics_.type(*left).kind;
            const TypeKind right_kind = semantics_.type(*right).kind;
            std::optional<TypeId> common;
            if (*left == *right) {
              common = *left;
            } else if (can_widen_numeric(left_kind, right_kind)) {
              common = *right;
            } else if (can_widen_numeric(right_kind, left_kind)) {
              common = *left;
            }
            numeric =
                common && expression.type == *common &&
                is_numeric_type(left_kind) && is_numeric_type(right_kind) &&
                (binary->operation != TokenKind::kPercent ||
                 (is_integer_type(left_kind) && is_integer_type(right_kind)));
            concatenate = binary->operation == TokenKind::kPlus &&
                          *left == semantics_.string_type() &&
                          *right == semantics_.string_type() &&
                          expression.type == semantics_.string_type();
          }
          if ((!numeric && !concatenate) || binary->left_is_presence_test ||
              binary->right_is_presence_test) {
            report(expression.range,
                   "arithmetic expression has incompatible HIR metadata");
          }
        }
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
      } else if (const auto* conversion =
                     std::get_if<HirIntegerConversionExpression>(
                         &expression.data)) {
        verify_expression(conversion->value, expression.range);
        const bool valid_mode =
            conversion->mode == IntegerConversionMode::kWrap ||
            conversion->mode == IntegerConversionMode::kSat;
        if (conversion->value.value < expressions.size() &&
            (!valid_mode ||
             !is_integer_type(semantics_.type(expression.type).kind) ||
             !is_integer_type(
                 semantics_.type(expressions[conversion->value.value].type)
                     .kind) ||
             expression.category != ValueCategory::kValue)) {
          report(expression.range,
                 "integer conversion has incompatible HIR metadata");
        }
      } else if (const auto* assignment =
                     std::get_if<HirAssignmentExpression>(&expression.data)) {
        verify_expression(assignment->target, expression.range);
        verify_expression(assignment->value, expression.range);
        const bool arithmetic =
            assignment->operation == TokenKind::kPlusEqual ||
            assignment->operation == TokenKind::kMinusEqual ||
            assignment->operation == TokenKind::kStarEqual ||
            assignment->operation == TokenKind::kSlashEqual ||
            assignment->operation == TokenKind::kPercentEqual;
        if (arithmetic) {
          const std::optional<TypeId> target =
              expression_type(assignment->target);
          const std::optional<TypeId> value =
              expression_type(assignment->value);
          bool numeric = false;
          bool concatenate = false;
          if (target && value && target->value < semantics_.types().size() &&
              value->value < semantics_.types().size()) {
            const TypeKind target_kind = semantics_.type(*target).kind;
            const TypeKind value_kind = semantics_.type(*value).kind;
            const bool assignable =
                *target == *value || can_widen_numeric(value_kind, target_kind);
            numeric =
                expression.type == *target && assignable &&
                is_numeric_type(target_kind) && is_numeric_type(value_kind) &&
                (assignment->operation != TokenKind::kPercentEqual ||
                 (is_integer_type(target_kind) && is_integer_type(value_kind)));
            concatenate = assignment->operation == TokenKind::kPlusEqual &&
                          *target == semantics_.string_type() &&
                          *value == semantics_.string_type() &&
                          expression.type == semantics_.string_type();
          }
          if ((!numeric && !concatenate) || !target ||
              hir_.storage.expression(assignment->target).category !=
                  ValueCategory::kMutableLocation ||
              expression.category != ValueCategory::kValue) {
            report(expression.range,
                   "arithmetic assignment has incompatible HIR metadata");
          }
        }
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
      for (std::size_t index = 0; index < file.fields.size(); ++index) {
        const auto& field = file.fields[index];
        verify_symbol(field.symbol, range);
        if (file.file.value < semantics_.files().size()) {
          const auto& fields = semantics_.file(file.file).fields;
          if (index >= fields.size() || fields[index] != field.symbol)
            report(range, "field identity/order does not match semantics");
        }
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
