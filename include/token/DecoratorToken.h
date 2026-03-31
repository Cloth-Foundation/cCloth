#pragma once
#include <cstdint>

#include "Token.h"

namespace cloth::token::decorator {
    using FileId = std::uint32_t;

    enum class DecoratorKeyword: std::uint16_t {
        None,

        Deprecated,
        Override,

        Prototype,
        Pure,
        Trait,

        Unused,
        Unstable,
    };

    class DecoratorToken {
    public:
        TokenKind kind = TokenKind::Error;
        DecoratorKeyword keyword = DecoratorKeyword::None;
        SourceSpan span{};
        std::string_view lexeme;
        std::uint32_t flags = 0;

        constexpr DecoratorToken() = default;

        constexpr DecoratorToken(TokenKind kind, const SourceSpan &span, std::string_view lexeme,
                            DecoratorKeyword keyword = DecoratorKeyword::None) noexcept
            : kind(kind), keyword(keyword), span(span), lexeme(lexeme) {
        }

        [[nodiscard]] constexpr bool is(token::TokenKind kind) const noexcept {
            return this->kind == kind;
        }

        // Safe owning string when the token needs to be stored beyond the lifetime of the original source buffer
        [[nodiscard]] std::string toString() const {
            return std::string(lexeme);
        }

        // Diagnostic-friendly location retrieval
        [[nodiscard]] constexpr SourceLocation location() const noexcept {
            return span.begin;
        }
    };
}
