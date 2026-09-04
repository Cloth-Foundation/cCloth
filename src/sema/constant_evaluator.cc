// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/sema/constant_evaluator.h"

#include "cloth/sema/canonical_identity.h"
#include "cloth/sema/numeric_types.h"
#include "cloth/sema/scalar_constants.h"

#include <algorithm>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cloth {
namespace {

std::vector<ExpressionId> children(const ExpressionData& data) {
  return std::visit(
      [](const auto& node) -> std::vector<ExpressionId> {
        if constexpr (requires {
                        node.left;
                        node.right;
                      })
          return {node.left, node.right};
        else if constexpr (requires {
                             node.target;
                             node.value;
                           }) {
          if constexpr (std::is_same_v<std::decay_t<decltype(node.target)>,
                                       ExpressionId>)
            return {node.target, node.value};
          else
            return {node.value};
        } else if constexpr (requires {
                               node.nullable;
                               node.fallback;
                             })
          return {node.nullable, node.fallback};
        else if constexpr (requires {
                             node.object;
                             node.index;
                           })
          return {node.object, node.index};
        else if constexpr (requires { node.object; })
          return {node.object};
        else if constexpr (requires { node.operand; })
          return {node.operand};
        else if constexpr (requires { node.value; })
          return {node.value};
        else if constexpr (requires { node.expression; })
          return {node.expression};
        else if constexpr (requires { node.arguments; }) {
          std::vector<ExpressionId> result{node.callee};
          result.insert(result.end(), node.arguments.begin(),
                        node.arguments.end());
          return result;
        } else if constexpr (requires { node.elements; })
          return node.elements;
        else
          return {};
      },
      data);
}

struct NumericLiteral {
  LiteralExpression literal;
  std::vector<TokenKind> signs;
};

std::optional<NumericLiteral> numeric_literal(const AstStorage& storage,
                                              ExpressionId id) {
  std::vector<TokenKind> signs;
  for (;;) {
    const auto& data = storage.expression(id).data;
    if (const auto* group = std::get_if<ParenthesizedExpression>(&data))
      id = group->expression;
    else if (const auto* unary = std::get_if<UnaryExpression>(&data)) {
      if (unary->operation != TokenKind::kPlus &&
          unary->operation != TokenKind::kMinus)
        return std::nullopt;
      signs.push_back(unary->operation);
      id = unary->operand;
    } else if (const auto* literal = std::get_if<LiteralExpression>(&data);
               literal && (literal->kind == LiteralKind::kInteger ||
                           literal->kind == LiteralKind::kFloat))
      return NumericLiteral{*literal, std::move(signs)};
    else
      return std::nullopt;
  }
}

}  // namespace

std::optional<std::size_t> preflight_constant_expression(
    const AstStorage& storage, ExpressionId root,
    DiagnosticEngine& diagnostics) {
  std::vector<std::pair<ExpressionId, std::size_t>> work{{root, 1}};
  std::size_t count = 0;
  std::optional<SourceRange> ineligible;
  while (!work.empty()) {
    const auto [id, depth] = work.back();
    work.pop_back();
    const auto& expression = storage.expression(id);
    const auto& data = expression.data;
    if (!ineligible && !std::holds_alternative<LiteralExpression>(data) &&
        !std::holds_alternative<IdentifierExpression>(data) &&
        !std::holds_alternative<MemberAccessExpression>(data) &&
        !std::holds_alternative<UnaryExpression>(data) &&
        !std::holds_alternative<BinaryExpression>(data) &&
        !std::holds_alternative<NumericConversionExpression>(data) &&
        !std::holds_alternative<IntegerConversionExpression>(data) &&
        !std::holds_alternative<ParenthesizedExpression>(data))
      ineligible = expression.range;
    if (!ineligible) {
      if (const auto* member = std::get_if<MemberAccessExpression>(&data)) {
        auto qualifier = member->object;
        for (std::size_t groups = 0; groups < kMaxConstantDepth; ++groups) {
          const auto* group = std::get_if<ParenthesizedExpression>(
              &storage.expression(qualifier).data);
          if (!group) break;
          qualifier = group->expression;
        }
        // Static fields/cases use a type name, not an instance/member path.
        if (!std::holds_alternative<IdentifierExpression>(
                storage.expression(qualifier).data))
          ineligible = expression.range;
      }
    }
    std::string_view error;
    if (++count > kMaxConstantNodes)
      error = "constant initializer exceeds 65536 expression nodes";
    else if (depth > kMaxConstantDepth)
      error = "constant expression exceeds nesting depth 256";
    else if (const auto* literal =
                 std::get_if<LiteralExpression>(&expression.data);
             literal &&
             (literal->kind == LiteralKind::kInteger ||
              literal->kind == LiteralKind::kFloat) &&
             literal->lexeme.size() > kMaxConstantLiteralBytes)
      error = "constant numeric literal exceeds 4096 bytes";
    if (!error.empty()) {
      diagnostics.error(expression.range, std::string{error});
      return std::nullopt;
    }
    const auto operands = children(expression.data);
    for (auto it = operands.rbegin(); it != operands.rend(); ++it)
      work.emplace_back(*it, depth + 1);
  }
  // Ineligible syntax cannot become valid through typing. Reject it before
  // recursive analysis of calls/aggregates, whose stacks are not part of the
  // scalar-expression contract. Reference eligibility is checked after binding.
  if (ineligible) {
    diagnostics.error(
        *ineligible,
        "static field initializer must be a scalar constant expression");
    return std::nullopt;
  }
  return count;
}

class ConstantEvaluator {
 public:
  ConstantEvaluator(std::span<const FileClassDecl* const> files,
                    SemanticModel& semantics, DiagnosticEngine& diagnostics)
      : files_(files), semantics_(semantics), diagnostics_(diagnostics) {}

  void run() {
    for (std::size_t file = 0; file < files_.size(); ++file) {
      const auto& syntax = *files_[file];
      for (std::size_t field = 0; field < syntax.fields.size(); ++field) {
        if (!syntax.fields[field].is_static) continue;
        const auto symbol = semantics_.file(FileId{file}).fields[field];
        nodes_.push_back(Node{
            symbol, FileId{file}, syntax.fields[field].initializer,
            canonical_symbol_identity(semantics_.symbol(symbol), semantics_,
                                      CanonicalMemberKind::kStaticField)});
      }
    }
    std::ranges::sort(nodes_, {}, &Node::identity);
    for (std::size_t i = 0; i < nodes_.size(); ++i)
      index_.emplace(nodes_[i].symbol.value, i);
    for (auto& node : nodes_) bind(node);
    for (std::size_t root = 0; root < nodes_.size(); ++root) visit(root);
  }

 private:
  enum class State { kUnevaluated, kEvaluating, kValid, kFailed };
  struct Edge {
    std::size_t target;
    SourceRange range;
  };
  struct Node {
    SymbolId symbol;
    FileId file;
    std::optional<ExpressionId> initializer;
    std::string identity;
    std::vector<Edge> edges{};
    State state{State::kUnevaluated};
  };

  void fail(Node& node) {
    node.state = State::kFailed;
    auto& symbol = semantics_.mutable_symbol(node.symbol);
    symbol.is_valid = false;
    symbol.static_constant.reset();
    semantics_.mutable_file(node.file).is_valid = false;
  }

  void bind(Node& node) {
    const auto& symbol = semantics_.symbol(node.symbol);
    if (!node.initializer || !symbol.is_valid || !symbol.is_final ||
        !is_scalar_constant_type(semantics_.type(symbol.type).kind)) {
      fail(node);
      return;
    }
    const auto& storage = files_[node.file.value]->storage;
    std::vector<ExpressionId> work{*node.initializer};
    while (!work.empty()) {
      const auto id = work.back();
      work.pop_back();
      const auto& expression = storage.expression(id);
      const auto& data = expression.data;
      const auto& state = semantics_.file(node.file).expressions[id.value];
      bool eligible =
          std::holds_alternative<LiteralExpression>(data) ||
          std::holds_alternative<ParenthesizedExpression>(data) ||
          std::holds_alternative<UnaryExpression>(data) ||
          std::holds_alternative<BinaryExpression>(data) ||
          std::holds_alternative<NumericConversionExpression>(data) ||
          std::holds_alternative<IntegerConversionExpression>(data);
      if (std::holds_alternative<IdentifierExpression>(data) ||
          std::holds_alternative<MemberAccessExpression>(data)) {
        eligible = false;
        if (state.symbol) {
          const auto& referenced = semantics_.symbol(*state.symbol);
          eligible = referenced.kind == SymbolKind::kEnumCase ||
                     (referenced.kind == SymbolKind::kField &&
                      referenced.is_static && referenced.is_final);
          if (eligible) {
            if (auto target = index_.find(state.symbol->value);
                target != index_.end())
              node.edges.push_back({target->second, expression.range});
            // Qualified type nodes are lookup syntax, not evaluated operands.
            continue;
          }
        }
      }
      if (!eligible ||
          !is_scalar_constant_type(semantics_.type(state.type).kind)) {
        diagnostics_.error(
            expression.range,
            "static field initializer must be a scalar constant expression");
        fail(node);
        break;
      }
      const auto operands = children(data);
      for (auto it = operands.rbegin(); it != operands.rend(); ++it)
        work.push_back(*it);
    }
    std::stable_sort(
        node.edges.begin(), node.edges.end(),
        [](const Edge& a, const Edge& b) { return a.target < b.target; });
    node.edges.erase(std::unique(node.edges.begin(), node.edges.end(),
                                 [](const Edge& a, const Edge& b) {
                                   return a.target == b.target;
                                 }),
                     node.edges.end());
  }

  void visit(std::size_t root) {
    if (nodes_[root].state != State::kUnevaluated) return;
    struct Frame {
      std::size_t node;
      std::size_t edge;
    };
    std::vector<Frame> stack{{root, 0}};
    nodes_[root].state = State::kEvaluating;
    while (!stack.empty()) {
      auto& frame = stack.back();
      auto& node = nodes_[frame.node];
      if (frame.edge < node.edges.size()) {
        const auto edge = node.edges[frame.edge++];
        auto& target = nodes_[edge.target];
        if (target.state == State::kUnevaluated) {
          target.state = State::kEvaluating;
          stack.push_back({edge.target, 0});
        } else if (target.state == State::kEvaluating) {
          diagnostics_.error(edge.range,
                             "cyclic static constant dependency involving '" +
                                 semantics_.symbol(target.symbol).name + "'");
          const auto begin =
              std::ranges::find(stack, edge.target, &Frame::node);
          std::size_t notes = 0;
          for (auto it = begin; it != stack.end(); ++it) {
            if (notes++ < 8) {
              const auto& path_node = nodes_[it->node];
              diagnostics_.note(path_node.edges[it->edge - 1].range,
                                "constant dependency through '" +
                                    semantics_.symbol(path_node.symbol).name +
                                    "'");
            }
            fail(nodes_[it->node]);
          }
          if (notes > 8)
            diagnostics_.note(edge.range,
                              std::to_string(notes - 8) +
                                  " additional cycle references omitted");
        }
        continue;
      }
      if (node.state != State::kFailed) {
        const bool valid =
            std::ranges::all_of(node.edges, [&](const Edge& edge) {
              return nodes_[edge.target].state == State::kValid;
            });
        if (valid) {
          const auto result = expression(node, *node.initializer);
          if (result) {
            const auto type = semantics_.symbol(node.symbol).type;
            const auto converted =
                convert_scalar(result->bits, semantics_.type(result->type).kind,
                               semantics_.type(type).kind);
            if (converted) {
              semantics_.mutable_symbol(node.symbol).static_constant =
                  ScalarConstant{type, *converted};
              node.state = State::kValid;
            } else {
              diagnostics_.error(
                  semantics_.symbol(node.symbol).range,
                  std::string{constant_error_message(converted.error())});
              fail(node);
            }
          } else
            fail(node);
        } else
          fail(node);
      }
      stack.pop_back();
    }
  }

  std::optional<ScalarConstant> expression(const Node& node, ExpressionId id) {
    const auto& storage = files_[node.file.value]->storage;
    const auto& syntax = storage.expression(id);
    const auto& state = semantics_.file(node.file).expressions[id.value];
    const auto kind = semantics_.type(state.type).kind;
    auto result = [&](ConstantBits value) -> std::optional<ScalarConstant> {
      if (value) return ScalarConstant{state.type, *value};
      diagnostics_.error(syntax.range,
                         std::string{constant_error_message(value.error())});
      return std::nullopt;
    };
    if (const auto literal = numeric_literal(storage, id))
      return result(scalar_signed_literal(literal->literal.kind,
                                          literal->literal.lexeme, kind,
                                          literal->signs));
    if (const auto* literal = std::get_if<LiteralExpression>(&syntax.data))
      return result(scalar_literal(literal->kind, literal->lexeme, kind));
    if (const auto* group = std::get_if<ParenthesizedExpression>(&syntax.data))
      return expression(node, group->expression);
    if (std::holds_alternative<IdentifierExpression>(syntax.data) ||
        std::holds_alternative<MemberAccessExpression>(syntax.data)) {
      const auto& symbol = semantics_.symbol(*state.symbol);
      if (symbol.kind == SymbolKind::kEnumCase)
        return ScalarConstant{symbol.type, *symbol.enum_tag};
      if (!symbol.static_constant)
        return result(std::unexpected(ConstantError::kInvalidOperation));
      return symbol.static_constant;
    }
    if (const auto* conversion =
            std::get_if<NumericConversionExpression>(&syntax.data)) {
      if (const auto literal = numeric_literal(storage, conversion->value))
        return result(scalar_signed_literal(literal->literal.kind,
                                            literal->literal.lexeme, kind,
                                            literal->signs));
      const auto operand = expression(node, conversion->value);
      return operand ? result(convert_scalar(
                           operand->bits, semantics_.type(operand->type).kind,
                           kind))
                     : std::nullopt;
    }
    if (const auto* conversion =
            std::get_if<IntegerConversionExpression>(&syntax.data)) {
      const auto operand = expression(node, conversion->value);
      const IntegerConversionMode mode = conversion->operation == "wrap"
                                             ? IntegerConversionMode::kWrap
                                             : IntegerConversionMode::kSat;
      return operand ? result(convert_integer_mode(
                           operand->bits, semantics_.type(operand->type).kind,
                           kind, mode))
                     : std::nullopt;
    }
    if (const auto* unary = std::get_if<UnaryExpression>(&syntax.data)) {
      const auto operand = expression(node, unary->operand);
      return operand
                 ? result(unary_scalar(unary->operation, kind, operand->bits))
                 : std::nullopt;
    }
    const auto& binary = std::get<BinaryExpression>(syntax.data);
    const auto left = expression(node, binary.left);
    if (!left) return std::nullopt;
    if ((binary.operation == TokenKind::kAmpersandAmpersand && !left->bits) ||
        (binary.operation == TokenKind::kPipePipe && left->bits))
      return result(left->bits != 0);
    const auto right = expression(node, binary.right);
    if (!right) return std::nullopt;
    const auto a = semantics_.type(left->type).kind;
    const auto b = semantics_.type(right->type).kind;
    if (binary.operation == TokenKind::kShiftLeft ||
        binary.operation == TokenKind::kShiftRight)
      return result(
          binary_scalar(binary.operation, a, left->bits, right->bits, b));
    const auto common = can_widen_numeric(a, b) ? b : a;
    const auto l = convert_scalar(left->bits, a, common);
    const auto r = convert_scalar(right->bits, b, common);
    if (!l || !r)
      return result(std::unexpected(ConstantError::kInvalidOperation));
    return result(binary_scalar(binary.operation, common, *l, *r, common));
  }

  std::span<const FileClassDecl* const> files_;
  SemanticModel& semantics_;
  DiagnosticEngine& diagnostics_;
  std::vector<Node> nodes_;
  std::map<std::size_t, std::size_t> index_;
};

void evaluate_static_constants(std::span<const FileClassDecl* const> files,
                               SemanticModel& semantics,
                               DiagnosticEngine& diagnostics) {
  ConstantEvaluator{files, semantics, diagnostics}.run();
}

}  // namespace cloth
