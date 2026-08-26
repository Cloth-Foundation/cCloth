#ifndef CLOTH_AST_AST_H_
#define CLOTH_AST_AST_H_

#include "cloth/lexer/token.h"
#include "cloth/sema/visibility.h"
#include "cloth/source/source_range.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cloth {

struct ExpressionId {
  std::size_t value;

  friend bool operator==(const ExpressionId&, const ExpressionId&) = default;
};

struct StatementId {
  std::size_t value;

  friend bool operator==(const StatementId&, const StatementId&) = default;
};

struct BlockId {
  std::size_t value;

  friend bool operator==(const BlockId&, const BlockId&) = default;
};

struct TypeSyntax {
  std::string_view name;
  bool is_primitive;
  SourceRange range;
};

enum class LiteralKind {
  kInteger,
  kFloat,
  kString,
  kCharacter,
  kBoolean,
  kNull,
};

struct InvalidExpression {};

struct IdentifierExpression {
  std::string_view name;
};

struct LiteralExpression {
  LiteralKind kind;
  std::string_view lexeme;
};

struct UnaryExpression {
  TokenKind operation;
  ExpressionId operand;
};

struct BinaryExpression {
  ExpressionId left;
  TokenKind operation;
  ExpressionId right;
};

struct AssignmentExpression {
  ExpressionId target;
  TokenKind operation;
  ExpressionId value;
};

struct MemberAccessExpression {
  ExpressionId object;
  std::string_view member;
};

struct CallExpression {
  ExpressionId callee;
  std::vector<ExpressionId> arguments;
};

struct ParenthesizedExpression {
  ExpressionId expression;
};

using ExpressionData =
    std::variant<InvalidExpression, IdentifierExpression, LiteralExpression,
                 UnaryExpression, BinaryExpression, AssignmentExpression,
                 MemberAccessExpression, CallExpression,
                 ParenthesizedExpression>;

struct Expression {
  SourceRange range;
  ExpressionData data;
};

struct InvalidStatement {};

struct LocalVariableStatement {
  TypeSyntax type;
  std::string_view name;
  std::optional<ExpressionId> initializer;
};

struct ReturnStatement {
  std::optional<ExpressionId> value;
};

struct ExpressionStatement {
  ExpressionId expression;
};

struct IfStatement {
  ExpressionId condition;
  BlockId then_block;
  std::optional<BlockId> else_block;
};

struct WhileStatement {
  ExpressionId condition;
  BlockId body;
};

struct BreakStatement {};

struct ContinueStatement {};

struct NestedBlockStatement {
  BlockId block;
};

using StatementData =
    std::variant<InvalidStatement, LocalVariableStatement, ReturnStatement,
                 ExpressionStatement, IfStatement, WhileStatement,
                 BreakStatement, ContinueStatement, NestedBlockStatement>;

struct Statement {
  SourceRange range;
  StatementData data;
};

struct Block {
  SourceRange range;
  std::vector<StatementId> statements;
  bool is_valid{true};
};

class AstStorage {
 public:
  [[nodiscard]] ExpressionId add_expression(Expression expression);
  [[nodiscard]] StatementId add_statement(Statement statement);
  [[nodiscard]] BlockId add_block(Block block);

  [[nodiscard]] const Expression& expression(ExpressionId id) const;
  [[nodiscard]] const Statement& statement(StatementId id) const;
  [[nodiscard]] const Block& block(BlockId id) const;

  [[nodiscard]] std::span<const Expression> expressions() const noexcept;
  [[nodiscard]] std::span<const Statement> statements() const noexcept;
  [[nodiscard]] std::span<const Block> blocks() const noexcept;

 private:
  std::vector<Expression> expressions_;
  std::vector<Statement> statements_;
  std::vector<Block> blocks_;
};

struct ParameterDecl {
  TypeSyntax type;
  std::string_view name;
  SourceRange range;
};

struct FieldDecl {
  TypeSyntax type;
  std::string_view name;
  Visibility visibility;
  std::optional<ExpressionId> initializer;
  SourceRange range;
  bool is_valid{true};
};

struct FunctionDecl {
  std::string_view name;
  Visibility visibility;
  std::vector<ParameterDecl> parameters;
  std::optional<TypeSyntax> return_type;
  BlockId body;
  SourceRange range;
  bool is_valid{true};
};

struct ConstructorDecl {
  std::string_view name;
  Visibility visibility;
  std::vector<ParameterDecl> parameters;
  BlockId body;
  SourceRange range;
  bool is_valid{true};
};

enum class DeclarationKind {
  kField,
  kFunction,
  kConstructor,
  kNestedType,
};

struct MemberReference {
  DeclarationKind kind;
  std::size_t index;

  friend bool operator==(const MemberReference&,
                         const MemberReference&) = default;
};

struct FileClassDecl {
  std::string name;
  std::string_view source_path;
  Visibility visibility;
  SourceRange range;
  AstStorage storage;
  std::vector<FieldDecl> fields;
  std::vector<FunctionDecl> functions;
  std::vector<ConstructorDecl> constructors;
  std::vector<MemberReference> member_order;
  bool is_valid{true};
};

[[nodiscard]] std::string_view declaration_kind_name(
    DeclarationKind kind) noexcept;

}  // namespace cloth

#endif  // CLOTH_AST_AST_H_
