#pragma once

#include <token/Token.h>

#include <cstdint>

namespace cloth::parser {
    enum class Precedence : std::uint8_t {
        None = 0,
        Assignment = 1,
        Equality = 2,
        Relational = 3,
        Additive = 4,
        Multiplicative = 5,
        Prefix = 6,
        Postfix = 7,
        Primary = 8,
    };

    [[nodiscard]] constexpr Precedence precedenceOf(token::Operator op) noexcept {
        switch (op) {
            case token::Operator::Assign:
            case token::Operator::PlusAssign:
            case token::Operator::MinusAssign:
            case token::Operator::StarAssign:
            case token::Operator::SlashAssign:
            case token::Operator::PercentAssign:
                return Precedence::Assignment;

            case token::Operator::Equal:
            case token::Operator::NotEqual:
                return Precedence::Equality;

            case token::Operator::Less:
            case token::Operator::LessEqual:
            case token::Operator::Greater:
            case token::Operator::GreaterEqual:
                return Precedence::Relational;

            case token::Operator::Plus:
            case token::Operator::Minus:
                return Precedence::Additive;

            case token::Operator::Star:
            case token::Operator::Slash:
            case token::Operator::Percent:
                return Precedence::Multiplicative;

            default:
                return Precedence::None;
        }
    }

    [[nodiscard]] constexpr bool isRightAssociative(token::Operator op) noexcept {
        switch (op) {
            case token::Operator::Assign:
            case token::Operator::PlusAssign:
            case token::Operator::MinusAssign:
            case token::Operator::StarAssign:
            case token::Operator::SlashAssign:
            case token::Operator::PercentAssign:
                return true;
            default:
                return false;
        }
    }
}
