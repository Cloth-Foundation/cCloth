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
  bool is_array{false};
  bool is_nullable{false};
  bool is_element_nullable{false};
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

struct SuperExpression {};

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

struct TypeTestExpression {
  ExpressionId value;
  TypeSyntax target;
};

struct CheckedCastExpression {
  ExpressionId value;
  TypeSyntax target;
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

struct MetaAccessExpression {
  ExpressionId object;
  std::string_view meta;
};

struct SafeMemberAccessExpression {
  ExpressionId object;
  std::string_view member;
};

struct NullCoalesceExpression {
  ExpressionId nullable;
  ExpressionId fallback;
};

struct NullAssertExpression {
  ExpressionId operand;
};

struct CallExpression {
  ExpressionId callee;
  std::vector<ExpressionId> arguments;
};

struct ArrayLiteralExpression {
  std::vector<ExpressionId> elements;
};

struct IndexExpression {
  ExpressionId object;
  ExpressionId index;
};

struct ParenthesizedExpression {
  ExpressionId expression;
};

using ExpressionData = std::variant<
    InvalidExpression, IdentifierExpression, LiteralExpression, SuperExpression,
    UnaryExpression, BinaryExpression, TypeTestExpression,
    CheckedCastExpression, AssignmentExpression, MemberAccessExpression,
    MetaAccessExpression, SafeMemberAccessExpression, NullCoalesceExpression,
    NullAssertExpression, CallExpression, ArrayLiteralExpression,
    IndexExpression, ParenthesizedExpression>;

struct Expression {
  SourceRange range;
  ExpressionData data;
};

struct InvalidStatement {};

struct LocalVariableStatement {
  std::optional<TypeSyntax> type;
  std::string_view name;
  std::optional<ExpressionId> initializer;
  bool is_final{false};
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

struct ForVariableDecl {
  std::optional<TypeSyntax> type;
  std::string_view name;
  SourceRange range;
  bool is_final{false};
};

struct ForStatement {
  ForVariableDecl variable;
  ExpressionId iterable;
  BlockId body;
};

struct BreakStatement {};

struct ContinueStatement {};

struct NestedBlockStatement {
  BlockId block;
};

using StatementData =
    std::variant<InvalidStatement, LocalVariableStatement, ReturnStatement,
                 ExpressionStatement, IfStatement, WhileStatement, ForStatement,
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
  bool is_final{false};
};

struct FieldDecl {
  TypeSyntax type;
  std::string_view name;
  Visibility visibility;
  std::optional<ExpressionId> initializer;
  SourceRange range;
  bool is_valid{true};
  bool is_final{false};
  bool is_static{false};
};

struct FunctionDecl {
  std::string_view name;
  Visibility visibility;
  std::vector<ParameterDecl> parameters;
  std::optional<TypeSyntax> return_type;
  BlockId body;
  SourceRange range;
  bool is_valid{true};
  bool is_static{false};
  bool is_override{false};
  bool is_abstract{false};
  bool is_final{false};
};

struct ConstructorInitializer {
  TypeSyntax base_type;
  std::vector<ExpressionId> arguments;
  SourceRange range;
  bool is_valid{true};
};

struct ConstructorDecl {
  std::string_view name;
  Visibility visibility;
  std::vector<ParameterDecl> parameters;
  std::optional<ConstructorInitializer> initializer;
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

enum class ImportKind {
  kType,
  kWildcard,
};

struct ImportDecl {
  ImportKind kind;
  std::string package_name;
  std::string type_name;
  std::string local_name;
  SourceRange range;
  bool is_valid{true};
};

enum class FileTypeKind {
  kClass,
  kInterface,
};

struct FileClassDecl {
  std::string name;
  std::string package_name;
  std::string qualified_name;
  std::string_view source_path;
  Visibility visibility;
  SourceRange range;
  std::vector<ImportDecl> imports;
  AstStorage storage;
  std::vector<FieldDecl> fields;
  std::vector<FunctionDecl> functions;
  std::vector<ConstructorDecl> constructors;
  std::vector<MemberReference> member_order;
  bool is_valid{true};
  std::optional<TypeSyntax> base_class{};
  bool has_explicit_class_declaration{false};
  bool is_abstract{false};
  bool is_sealed{false};
  FileTypeKind kind{FileTypeKind::kClass};
  std::vector<TypeSyntax> interfaces{};
};

[[nodiscard]] std::string_view declaration_kind_name(
    DeclarationKind kind) noexcept;

}  // namespace cloth

#endif  // CLOTH_AST_AST_H_
