#pragma once

#include <lexer/Lexer.h>
#include <ast/Expr.h>
#include <ast/Stmt.h>
#include <parser/ParseResult.h>
#include <token/Token.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

namespace cloth::parser {
    struct DiagnosticSink {
        virtual ~DiagnosticSink() = default;
        virtual void error(token::SourceLocation location, std::string_view message) = 0;
    };

    class Parser final {
    public:
        Parser(lexer::Lexer &lexer, DiagnosticSink &diagnostics);

        [[nodiscard]] ParseResult parseProgram();

    private:
        lexer::Lexer *lexer_ = nullptr;
        DiagnosticSink *diagnostics_ = nullptr;
        bool had_error_ = false;

        // --- Token helpers ---
        [[nodiscard]] const lexer::LexedToken &peek(std::size_t n = 0);
        [[nodiscard]] const token::Token &peekToken(std::size_t n = 0);
        [[nodiscard]] bool eof();
        lexer::LexedToken next();

        [[nodiscard]] bool check(token::TokenKind kind, std::size_t n = 0);
        [[nodiscard]] bool checkOperator(token::Operator op, std::size_t n = 0);
        [[nodiscard]] bool checkKeyword(token::Keyword kw, std::size_t n = 0);

        std::optional<token::Token> tryConsumeOperator(token::Operator op);
        std::optional<token::Token> tryConsumeKeyword(token::Keyword kw);
        std::optional<token::Token> tryConsume(token::TokenKind kind);

        token::Token expectOperator(token::Operator op, std::string_view message);
        token::Token expect(token::TokenKind kind, std::string_view message);

        void errorHere(std::string_view message);
        void synchronizeStatement();

        // --- Parsing ---
        ast::StmtPtr parseStatement();
        ast::StmtPtr parseVarDeclStatement();
        ast::StmtPtr parseExprStatement();

        ast::ExprPtr parseExpression();
        ast::ExprPtr parseExpressionPrec(std::uint8_t minPrec);
        ast::ExprPtr parsePrefix();
        ast::ExprPtr parsePostfix(ast::ExprPtr left);
        ast::ExprPtr parsePrimary();
    };
}
