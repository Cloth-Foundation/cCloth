#pragma once

#include <token/Token.h>
#include <token/MetaToken.h>
#include <cstdint>
#include <string_view>
#include <array>
#include <vector>

namespace cloth::lexer {
    // Owned by the compiler frontend, represents a source file or input buffer
    struct SourceBuffer {
        token::FileId file = 0;
        std::string_view text; // must outlive any tokens produced from this buffer
        std::string_view filename; // for diagnostics
    };

    // Diagnostic sink interface for reporting errors and warnings during lexing
    struct DiagnosticSink {
        virtual ~DiagnosticSink() = default;

        virtual void error(token::SourceLocation location, std::string_view message) = 0;

        virtual void warning(token::SourceLocation location, std::string_view message) = 0;
    };

    struct LexerOptions {
        bool emit_whitespace = false; // whether to produce tokens for whitespace
        bool emit_comments = false; // whether to produce tokens for comments
        bool keep_trivia = false; // whether to keep trivia (whitespace/comments) in the token stream
        bool allow_unicode_identifiers = true; // whether to allow Unicode characters in identifiers

        std::uint32_t max_string_literal_bytes = 1u << 20;
        // 1 MiB limit for string literals to prevent excessive memory usage
        // TODO: We can remove this in future. We currently do not have any memory management system so we are limiting for now.
    };

    struct TriviaPiece {
        token::TokenKind kind = token::TokenKind::Whitespace; // Whitespace or Comment
        token::SourceSpan span{};
        std::string_view lexeme{};
    };

    struct Trivia {
        std::vector<TriviaPiece> leading;
        std::vector<TriviaPiece> trailing;

        void clear() {
            leading.clear();
            trailing.clear();
        }
    };

    // Optionally hand to parser
    struct LexedToken {
        token::Token token;
        Trivia trivia; // Leading and trailing trivia (whitespace/comments)
    };

    class Lexer final {
    public:
        Lexer(const SourceBuffer &buffer, DiagnosticSink &diagnostics, LexerOptions options = {});

        // Parser Interface
        const LexedToken &peek(std::size_t n = 0); // Lookahead without consuming
        LexedToken next(); // Consume and return the next token
        bool eof(); // Check if we've reached the end of the token stream

        // Utility
        [[nodiscard]] token::SourceLocation location() const noexcept {
            return location_;
        }

        [[nodiscard]] token::FileId file() const noexcept {
            return buffer_.file;
        }

    private:
        SourceBuffer buffer_;
        DiagnosticSink &diagnostics_;
        LexerOptions options_;

        const char *begin_ = nullptr; // Start of the current token
        const char *current_ = nullptr; // Current position in the buffer
        const char *end_ = nullptr; // End of the buffer

        // Current location tracking
        token::SourceLocation location_{};

        // Token start markers
        const char *token_begin_ = nullptr;
        token::SourceLocation token_location_{};

        // Lookahead ring buffer
        // TODO: I hate using a fixed-size lookahead buffer, but it simplifies the implementation for now. We can always switch to a dynamic structure if needed.
        // TODO: The naming here is fucking stupid.
        static constexpr std::size_t kLookahead = 8;
        std::array<LexedToken, kLookahead> la_{};
        std::size_t la_head_ = 0; // Points to the next slot to fill in the lookahead buffer
        std::size_t la_size_ = 0; // Number of valid tokens in the lookahead buffer

    private:
        // --- Lookahead Helpers ---
        void fillLookahead(std::size_t need);

        LexedToken lexOne();

        // --- Cursor primitives ---
        [[nodiscard]] bool atEnd() const noexcept {
            return current_ >= end_;
        }

        [[nodiscard]] char current() const noexcept {
            return atEnd() ? '\0' : *current_;
        }

        [[nodiscard]] char lookaheadChar(std::size_t n = 1) const noexcept {
            const char *p = current_ + n;
            return (p >= end_) ? '\0' : *p;
        }

        void advance() noexcept;

        void advanceN(std::size_t n) noexcept;

        bool match(char expected) noexcept;

        bool match2(char a, char b) noexcept;

        bool matchString(std::string_view expected) noexcept;

        // --- Lifecycle ---
        void beginToken() noexcept;

        [[nodiscard]] token::SourceSpan endTokenSpan() const noexcept;

        [[nodiscard]] std::string_view tokenLexeme() const noexcept;

        [[nodiscard]] token::Token makeToken(token::TokenKind kind) const;

        [[nodiscard]] token::Token makeToken(token::TokenKind kind, token::Keyword keyword) const;

        [[nodiscard]] token::Token makeToken(token::TokenKind kind, token::Operator op) const;

        [[nodiscard]] token::Token makeErrorToken(std::string_view message) const;

        [[nodiscard]] meta_token::MetaToken makeMetaToken(token::TokenKind kind) const;

        // --- Classification ---
        static bool isWhitespace(char c) noexcept;

        static bool isNewline(char c) noexcept;

        static bool isDigit(char c) noexcept;

        static bool isHexDigit(char c) noexcept;

        static bool isAlpha(char c) noexcept;

        static bool isIdentStart(char c) noexcept;

        static bool isIdentContinue(char c) noexcept;

        // --- Trivia handling ---
        void consumeTrivia(std::vector<TriviaPiece> &outLeading);

        void maybeConsumeTrailingTrivia(Trivia &outTrailing);

        [[nodiscard]] LexedToken emit(LexedToken token) const;

        // --- Scanners ---
        LexedToken scanEndOfFile();

        LexedToken scanIdentifierOrKeyword();

        LexedToken scanNumber();

        LexedToken scanStringLiteral(char quote);

        LexedToken scanOperatorOrPunctuation();

        LexedToken scanCommentOrSlashOperator();

        LexedToken scanWhitespaceToken(); // only when emit_whitespace is true

        // --- Sub-Helpers ---
        static token::Keyword resolveKeyword(std::string_view ident) noexcept;

        static token::Operator resolveOperator(std::string_view op) noexcept;

        static meta_token::MetaKeyword resolveMetaToken(std::string_view ident) noexcept;

        bool consumeLineComment();

        bool consumeBlockComment(); // including nested comments
        bool consumeEscapeSequence(); // inside string

        // --- Location ---
        void bumpLocation(char consumed) noexcept;

        std::uint32_t offsetFromBegin(const char *p) const noexcept {
            return static_cast<std::uint32_t>(p - begin_);
        }
    };
} // namespace cloth::lexer
