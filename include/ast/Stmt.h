#pragma once

#include <ast/Expr.h>
#include <token/Token.h>

#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace cloth::ast {
    struct Stmt;
    using StmtPtr = std::unique_ptr<Stmt>;

    struct VarDeclStmt {
        std::optional<token::Token> constToken;
        token::Token specifierToken;
        token::Token nameToken;
        std::optional<token::Token> equalsToken;
        ExprPtr initializer;
        token::Token semicolon;
    };

    struct ExprStmt {
        ExprPtr expr;
        token::Token semicolon;
    };

    struct Stmt {
        using Variant = std::variant<VarDeclStmt, ExprStmt>;
        Variant node;

        template <class T>
        explicit Stmt(T v) : node(std::move(v)) {
        }
    };
}
