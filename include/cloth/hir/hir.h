#ifndef CLOTH_HIR_HIR_H_
#define CLOTH_HIR_HIR_H_

#include "cloth/ast/ast.h"
#include "cloth/lexer/token.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace cloth {

struct HirExpressionId {
  std::size_t value;

  friend bool operator==(const HirExpressionId&,
                         const HirExpressionId&) = default;
};

struct HirStatementId {
  std::size_t value;

  friend bool operator==(const HirStatementId&,
                         const HirStatementId&) = default;
};

struct HirBlockId {
  std::size_t value;

  friend bool operator==(const HirBlockId&, const HirBlockId&) = default;
};

struct HirInvalidExpression {};

struct HirLiteralExpression {
  LiteralKind kind;
  std::string lexeme;
};

struct HirSymbolExpression {
  SymbolId symbol;
};

struct HirTypeExpression {
  TypeId type;
};

struct HirSuperExpression {};

struct HirUnaryExpression {
  TokenKind operation;
  HirExpressionId operand;
  bool operand_is_presence_test{false};
};

struct HirUpdateExpression {
  TokenKind operation;
  HirExpressionId operand;
  bool is_postfix{false};
};

struct HirBinaryExpression {
  HirExpressionId left;
  TokenKind operation;
  HirExpressionId right;
  bool left_is_presence_test{false};
  bool right_is_presence_test{false};
};

struct HirTypeTestExpression {
  HirExpressionId value;
  TypeId target;
};

struct HirCheckedCastExpression {
  HirExpressionId value;
  TypeId target;
};

struct HirAssignmentExpression {
  HirExpressionId target;
  TokenKind operation;
  HirExpressionId value;
};

struct HirMemberExpression {
  HirExpressionId object;
  std::optional<SymbolId> member;
};

struct HirSafeMemberExpression {
  HirExpressionId object;
  std::optional<SymbolId> member;
};

struct HirNullCoalesceExpression {
  HirExpressionId nullable;
  HirExpressionId fallback;
};

struct HirNullAssertExpression {
  HirExpressionId operand;
};

struct HirCallExpression {
  HirExpressionId callee;
  std::optional<SymbolId> callable;
  std::vector<HirExpressionId> arguments;
  bool is_base_qualified{false};
  std::optional<FileId> interface_dispatch{};
};

struct HirArrayLiteralExpression {
  TypeId element_type;
  std::vector<HirExpressionId> elements;
};

struct HirIndexExpression {
  HirExpressionId object;
  HirExpressionId index;
};

struct HirArrayLengthExpression {
  HirExpressionId array;
};

enum class StringMetaQuery {
  kLength,
  kByteLength,
  kIsEmpty,
};

struct HirStringMetaExpression {
  HirExpressionId string;
  StringMetaQuery query;
};

struct HirObjectMetaExpression {
  HirExpressionId object;
};

struct HirGroupedExpression {
  HirExpressionId expression;
};

using HirExpressionData = std::variant<
    HirInvalidExpression, HirLiteralExpression, HirSymbolExpression,
    HirTypeExpression, HirSuperExpression, HirUnaryExpression,
    HirUpdateExpression, HirBinaryExpression, HirTypeTestExpression,
    HirCheckedCastExpression, HirAssignmentExpression, HirMemberExpression,
    HirSafeMemberExpression, HirNullCoalesceExpression, HirNullAssertExpression,
    HirCallExpression, HirArrayLiteralExpression, HirIndexExpression,
    HirArrayLengthExpression, HirStringMetaExpression, HirObjectMetaExpression,
    HirGroupedExpression>;

struct HirExpression {
  TypeId type;
  SourceRange range;
  HirExpressionData data;
};

struct HirInvalidStatement {};

struct HirLocalStatement {
  std::optional<SymbolId> symbol;
  std::optional<HirExpressionId> initializer;
};

struct HirReturnStatement {
  std::optional<HirExpressionId> value;
};

struct HirExpressionStatement {
  HirExpressionId expression;
};

struct HirIfStatement {
  HirExpressionId condition;
  HirBlockId then_block;
  std::optional<HirBlockId> else_block;
  bool condition_is_presence_test{false};
};

struct HirWhileStatement {
  HirExpressionId condition;
  HirBlockId body;
  bool condition_is_presence_test{false};
};

struct HirForEachStatement {
  std::optional<SymbolId> variable;
  HirExpressionId iterable;
  HirBlockId body;
};

struct HirForStatement {
  std::optional<HirStatementId> initializer;
  std::optional<HirExpressionId> condition;
  std::vector<HirExpressionId> updates;
  HirBlockId body;
  bool condition_is_presence_test{false};
};

struct HirBreakStatement {};

struct HirContinueStatement {};

struct HirNestedBlockStatement {
  HirBlockId block;
};

using HirStatementData =
    std::variant<HirInvalidStatement, HirLocalStatement, HirReturnStatement,
                 HirExpressionStatement, HirIfStatement, HirWhileStatement,
                 HirForEachStatement, HirForStatement, HirBreakStatement,
                 HirContinueStatement, HirNestedBlockStatement>;

struct HirStatement {
  SourceRange range;
  HirStatementData data;
};

struct HirBlock {
  SourceRange range;
  std::vector<HirStatementId> statements;
};

class HirStorage {
 public:
  [[nodiscard]] HirExpressionId add_expression(HirExpression expression);
  [[nodiscard]] HirStatementId add_statement(HirStatement statement);
  [[nodiscard]] HirBlockId add_block(HirBlock block);

  [[nodiscard]] const HirExpression& expression(HirExpressionId id) const;
  [[nodiscard]] const HirStatement& statement(HirStatementId id) const;
  [[nodiscard]] const HirBlock& block(HirBlockId id) const;

  [[nodiscard]] std::span<const HirExpression> expressions() const noexcept;
  [[nodiscard]] std::span<const HirStatement> statements() const noexcept;
  [[nodiscard]] std::span<const HirBlock> blocks() const noexcept;

 private:
  std::vector<HirExpression> expressions_;
  std::vector<HirStatement> statements_;
  std::vector<HirBlock> blocks_;
};

struct HirField {
  SymbolId symbol;
  std::optional<HirExpressionId> initializer;
};

struct HirConstructorInitializer {
  SymbolId constructor;
  std::vector<HirExpressionId> arguments;
  SourceRange range;
};

struct HirCallable {
  SymbolId symbol;
  std::optional<HirConstructorInitializer> initializer;
  HirBlockId body;
};

struct HirFileClass {
  FileId file;
  SymbolId symbol;
  std::optional<FileId> base_file;
  std::vector<HirField> fields;
  std::vector<HirCallable> functions;
  std::vector<HirCallable> constructors;
  std::vector<MemberReference> member_order;
};

struct HirModule {
  HirStorage storage;
  std::vector<HirFileClass> files;
};

[[nodiscard]] HirModule lower_to_hir(
    std::span<const FileClassDecl* const> files,
    const SemanticModel& semantics);

}  // namespace cloth

#endif  // CLOTH_HIR_HIR_H_
