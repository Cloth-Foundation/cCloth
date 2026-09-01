// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/ast/ast.h"

#include <utility>

namespace cloth {

ExpressionId AstStorage::add_expression(Expression expression) {
  const ExpressionId id{expressions_.size()};
  expressions_.push_back(std::move(expression));
  return id;
}

StatementId AstStorage::add_statement(Statement statement) {
  const StatementId id{statements_.size()};
  statements_.push_back(std::move(statement));
  return id;
}

BlockId AstStorage::add_block(Block block) {
  const BlockId id{blocks_.size()};
  blocks_.push_back(std::move(block));
  return id;
}

const Expression& AstStorage::expression(ExpressionId id) const {
  return expressions_.at(id.value);
}

const Statement& AstStorage::statement(StatementId id) const {
  return statements_.at(id.value);
}

const Block& AstStorage::block(BlockId id) const {
  return blocks_.at(id.value);
}

std::span<const Expression> AstStorage::expressions() const noexcept {
  return expressions_;
}

std::span<const Statement> AstStorage::statements() const noexcept {
  return statements_;
}

std::span<const Block> AstStorage::blocks() const noexcept { return blocks_; }

std::string_view declaration_kind_name(DeclarationKind kind) noexcept {
  switch (kind) {
    case DeclarationKind::kField:
      return "field";
    case DeclarationKind::kFunction:
      return "function";
    case DeclarationKind::kConstructor:
      return "constructor";
    case DeclarationKind::kNestedType:
      return "nested type";
  }
  return "unknown";
}

}  // namespace cloth
