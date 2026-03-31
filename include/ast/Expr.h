#pragma once

#include <token/Token.h>

#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cloth::ast {
    struct Expr;
    using ExprPtr = std::unique_ptr<Expr>;

    struct NumberLiteralExpr {
        token::Token token;
    };

    struct IdentifierExpr {
        token::Token token;
    };

    struct UnaryExpr {
        token::Token opToken;
        token::Operator op = token::Operator::None;
        ExprPtr operand;
    };

    struct BinaryExpr {
        ExprPtr left;
        token::Token opToken;
        token::Operator op = token::Operator::None;
        ExprPtr right;
    };

    struct CallExpr {
        ExprPtr callee;
        token::Token leftParen;
        std::vector<ExprPtr> args;
        token::Token rightParen;
    };

    struct ParenExpr {
        token::Token leftParen;
        ExprPtr expr;
        token::Token rightParen;
    };

    struct Expr {
        using Variant = std::variant<NumberLiteralExpr, IdentifierExpr, UnaryExpr, BinaryExpr, CallExpr, ParenExpr>;
        Variant node;

        template <class T>
        explicit Expr(T v) : node(std::move(v)) {
        }
    };
}
