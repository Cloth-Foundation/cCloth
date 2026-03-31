#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <token/Token.h>

/// MetaTokens are a special kind of token used to represent compile-time metadata queries (e.g.,
/// LENGTH(int), TYPEOF(x), etc.) that can be evaluated by the compiler during parsing or semantic
/// analysis. They carry additional information about the specific metadata being queried (e.g., the
/// type or expression involved) and are distinct from regular tokens produced by the lexer.

namespace cloth::meta_token {
    using FileId = std::uint32_t;

    enum class MetaKeyword : std::uint16_t {
        None,

        LENGTH,
        SIZEOF,
        TO_STRING,
        TYPEOF,
        ALIGNOF,
        MEMSPACE,

        TO_BYTES,
        TO_BITS,
        DEFAULT, // Default initializer value for a type (e.g. DEFAULT(int) == 0, DEFAULT(bool) == false, etc.)

        // These are for integer types only and can be used to query the max/min values of a type (e.g. LENGTH(int) == 32, MAX(int) == 2147483647, MIN(int) == -2147483648)
        MAX,
        MIN
    };

    class MetaToken final {
    public:
        token::TokenKind kind = token::TokenKind::Error;
        MetaKeyword keyword = MetaKeyword::None;
        token::SourceSpan span{};

        // Lexeme view into the original source buffer
        // Points into the memory owned by the SourceManager or input buffer.
        std::string_view lexeme;

        // Literal payload fields (avoids allocations in the lexer)
        std::uint32_t flags = 0;

        constexpr MetaToken() = default;

        constexpr MetaToken(token::TokenKind kind, const token::SourceSpan &span, std::string_view lexeme,
                            MetaKeyword keyword = MetaKeyword::None) noexcept
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
        [[nodiscard]] constexpr token::SourceLocation location() const noexcept {
            return span.begin;
        }
    };
}
