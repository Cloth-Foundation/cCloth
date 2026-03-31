#pragma once

#include <cstdint>
#include <string>
#include <string_view>


namespace cloth::token {
    using FileId = std::uint32_t;

    enum class TokenKind : std::uint16_t {
        // Common
        EndOfFile,
        Error,

        // Literals
        Identifier,
        Number,
        String,

        // Punctuation
        Operator,
        Punctuation,

        // Types
        Keyword,
        Whitespace,
        Comment,

        // Meta-access tokens
        Meta,
        Decorator,
    };

    enum class Keyword : std::uint16_t {
        None,

        If, Else, For, While, Do, Switch, Case, Default,
        Return, Break, Continue,
        Func, Struct, Enum, Interface, Class,
        Var, Const,
        As, In, Or, And, Is,
        Null, True, False,

        Import,

        Static,
        Public, Private, Internal,
        Module,

        Try, Catch, Finally, Throw,

        I8, I16, I32, I64,
        U8, U16, U32, U64,
        F32, F64,
        String, Char, Bool,
        Bit, Byte,
        Void,
        Any,

        Defer, Async, Await,
        Atomic, Shared, Owned,
        Delete, New, This, Super,

        Get, Set, // C# inspired, which this is probably the only part of the language I like.
        Yield, Maybe,
    };

    enum class Operator : std::uint16_t {
        None,

        Plus, Minus, Star, Slash, Percent,
        PlusPlus, MinusMinus,
        Assign,
        PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
        Equal, NotEqual, Less, Greater, LessEqual, GreaterEqual,
        Amp, Pipe,
        Bang, Tilde,
        Dot, Comma, Semicolon, Colon,
        Arrow,
        LeftParen, RightParen,
        LeftBrace, RightBrace,
        LeftBracket, RightBracket,

        At, Hash, Dollar, Question,

        ColonColon, DotDot, DotDotDot,
        ReturnArrow,
    };

    struct SourceLocation {
        FileId file = 0;
        std::uint32_t offset = 0; // byte offset from start of file/buffer
        std::uint32_t line = 1; // line number (1-based)
        std::uint32_t column = 1; // column number (1-based)

        constexpr SourceLocation() = default;

        constexpr SourceLocation(FileId file, std::uint32_t offset, std::uint32_t line, std::uint32_t column)
            : file(file), offset(offset), line(line), column(column) {
        }
    };

    struct SourceSpan {
        SourceLocation begin{};
        SourceLocation end{};

        constexpr SourceSpan() = default;

        constexpr SourceSpan(SourceLocation begin, SourceLocation end) : begin(begin), end(end) {
        }

        [[nodiscard]] constexpr bool valid() const noexcept {
            return begin.file == end.file && end.offset >= begin.offset;
        }

        [[nodiscard]] constexpr std::uint32_t length() const noexcept {
            return valid() ? end.offset - begin.offset : 0;
        }
    };

    class Token final {
    public:
        TokenKind kind = TokenKind::Error;
        Keyword keyword = Keyword::None;
        Operator op = Operator::None;
        SourceSpan span{};

        // Lexeme view into the original source buffer
        // Points into the memory owned by the SourceManager or input buffer.
        std::string_view lexeme;

        // Literal payload fields (avoids allocations in the lexer)
        std::uint32_t flags = 0;

        constexpr Token() = default;

        constexpr Token(TokenKind kind, const SourceSpan &span, std::string_view lexeme, Keyword keyword = Keyword::None,
                        Operator op = Operator::None) noexcept
            : kind(kind), keyword(keyword), op(op), span(span), lexeme(lexeme) {
        }

        [[nodiscard]] constexpr bool is(TokenKind kind) const noexcept {
            return this->kind == kind;
        }

        // Safe owning string when the token needs to be stored beyond the lifetime of the
        // original source buffer
        [[nodiscard]] std::string toString() const {
            return std::string(lexeme);
        }

        // Diagnostic-friendly location retrieval
        [[nodiscard]] constexpr SourceLocation location() const noexcept {
            return span.begin;
        }
    };
} // namespace cloth::token
