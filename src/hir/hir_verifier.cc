#include "cloth/hir/hir_verifier.h"

#include "cloth/ast/ast.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_location.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

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
    return is_valid_;
  }

 private:
  void verify_expressions() {
    const auto expressions = hir_.storage.expressions();
    for (const HirExpression& expression : expressions) {
      verify_type(expression.type, expression.range);
      if (expression.type == semantics_.void_type() &&
          !is_valid_void_expression(expression)) {
        report(expression.range,
               "void expression is not a call or grouped void call");
      }
      if (const auto* symbol =
              std::get_if<HirSymbolExpression>(&expression.data)) {
        verify_symbol(symbol->symbol, expression.range);
      } else if (const auto* type =
                     std::get_if<HirTypeExpression>(&expression.data)) {
        verify_type(type->type, expression.range);
      } else if (const auto* unary =
                     std::get_if<HirUnaryExpression>(&expression.data)) {
        verify_expression(unary->operand, expression.range);
      } else if (const auto* binary =
                     std::get_if<HirBinaryExpression>(&expression.data)) {
        verify_expression(binary->left, expression.range);
        verify_expression(binary->right, expression.range);
      } else if (const auto* test =
                     std::get_if<HirTypeTestExpression>(&expression.data)) {
        verify_expression(test->value, expression.range);
        verify_type(test->target, expression.range);
      } else if (const auto* cast =
                     std::get_if<HirCheckedCastExpression>(&expression.data)) {
        verify_expression(cast->value, expression.range);
        verify_type(cast->target, expression.range);
      } else if (const auto* assignment =
                     std::get_if<HirAssignmentExpression>(&expression.data)) {
        verify_expression(assignment->target, expression.range);
        verify_expression(assignment->value, expression.range);
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
      } else if (const auto* grouped =
                     std::get_if<HirGroupedExpression>(&expression.data)) {
        verify_expression(grouped->expression, expression.range);
      }
    }
  }

  bool is_valid_void_expression(const HirExpression& expression) const {
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
                     std::get_if<HirForStatement>(&statement.data)) {
        verify_optional_symbol(for_statement->variable, statement.range);
        verify_expression(for_statement->iterable, statement.range);
        verify_block(for_statement->body, statement.range);
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
    if (hir_.files.size() != semantics_.files().size()) {
      report(fallback_range(), "file count does not match the semantic model");
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
