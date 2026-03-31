#include <parser/Parser.h>

#include <ast/Expr.h>
#include <ast/Program.h>
#include <ast/Stmt.h>
#include <parser/Precedence.h>

#include <memory>

namespace cloth::parser {
    Parser::Parser(lexer::Lexer &lexer, DiagnosticSink &diagnostics)
        : lexer_(&lexer), diagnostics_(&diagnostics) {
    }

    ParseResult Parser::parseProgram() {
        ast::Program program;
        while (!eof()) {
            auto stmt = parseStatement();
            if (stmt) program.statements.push_back(std::move(stmt));
            else break;

            if (had_error_) break;
        }

        ParseResult out;
        out.program = std::move(program);
        out.had_error = had_error_;
        return out;
    }

    const lexer::LexedToken &Parser::peek(std::size_t n) {
        return lexer_->peek(n);
    }

    const token::Token &Parser::peekToken(std::size_t n) {
        return peek(n).token;
    }

    bool Parser::eof() {
        return lexer_->eof();
    }

    lexer::LexedToken Parser::next() {
        return lexer_->next();
    }

    bool Parser::check(token::TokenKind kind, std::size_t n) {
        return peekToken(n).kind == kind;
    }

    bool Parser::checkOperator(token::Operator op, std::size_t n) {
        const auto &t = peekToken(n);
        return (t.kind == token::TokenKind::Operator || t.kind == token::TokenKind::Punctuation) && t.op == op;
    }

    bool Parser::checkKeyword(token::Keyword kw, std::size_t n) {
        const auto &t = peekToken(n);
        return t.kind == token::TokenKind::Keyword && t.keyword == kw;
    }

    std::optional<token::Token> Parser::tryConsumeOperator(token::Operator op) {
        if (!checkOperator(op)) return std::nullopt;
        return next().token;
    }

    std::optional<token::Token> Parser::tryConsumeKeyword(token::Keyword kw) {
        if (!checkKeyword(kw)) return std::nullopt;
        return next().token;
    }

    std::optional<token::Token> Parser::tryConsume(token::TokenKind kind) {
        if (!check(kind)) return std::nullopt;
        return next().token;
    }

    token::Token Parser::expectOperator(token::Operator op, std::string_view message) {
        if (auto t = tryConsumeOperator(op)) return *t;
        errorHere(message);
        return peekToken(0);
    }

    token::Token Parser::expect(token::TokenKind kind, std::string_view message) {
        if (auto t = tryConsume(kind)) return *t;
        errorHere(message);
        return peekToken(0);
    }

    void Parser::errorHere(std::string_view message) {
        had_error_ = true;
        diagnostics_->error(peekToken(0).location(), message);
    }

    void Parser::synchronizeStatement() {
        while (!eof()) {
            if (checkOperator(token::Operator::Semicolon)) {
                next();
                return;
            }

            if (checkKeyword(token::Keyword::If) || checkKeyword(token::Keyword::For) || checkKeyword(token::Keyword::While) ||
                checkKeyword(token::Keyword::Return) || checkKeyword(token::Keyword::Func) || checkKeyword(token::Keyword::Class) ||
                checkKeyword(token::Keyword::Struct) || checkKeyword(token::Keyword::Var) || checkKeyword(token::Keyword::Const) ||
                checkKeyword(token::Keyword::I32) || checkKeyword(token::Keyword::I64) || checkKeyword(token::Keyword::U32) ||
                checkKeyword(token::Keyword::F32) || checkKeyword(token::Keyword::F64) || checkKeyword(token::Keyword::Bool) ||
                checkKeyword(token::Keyword::String)) {
                return;
            }

            next();
        }
    }

    static bool isTypeKeyword(token::Keyword kw) {
        switch (kw) {
            case token::Keyword::I8:
            case token::Keyword::I16:
            case token::Keyword::I32:
            case token::Keyword::I64:
            case token::Keyword::U8:
            case token::Keyword::U16:
            case token::Keyword::U32:
            case token::Keyword::U64:
            case token::Keyword::F32:
            case token::Keyword::F64:
            case token::Keyword::Bool:
            case token::Keyword::String:
            case token::Keyword::Char:
            case token::Keyword::Byte:
            case token::Keyword::Bit:
            case token::Keyword::Any:
            case token::Keyword::Void:
                return true;
            default:
                return false;
        }
    }

    ast::StmtPtr Parser::parseStatement() {
        // Variable declaration starts with optional `const`, then either a type keyword (i32, i64, ...)
        // or `var` for inferred type.
        if (checkKeyword(token::Keyword::Const)) {
            return parseVarDeclStatement();
        }

        if (checkKeyword(token::Keyword::Var)) {
            return parseVarDeclStatement();
        }

        if (check(token::TokenKind::Keyword) && isTypeKeyword(peekToken(0).keyword)) {
            return parseVarDeclStatement();
        }

        return parseExprStatement();
    }

    ast::StmtPtr Parser::parseVarDeclStatement() {
        std::optional<token::Token> constTok;
        if (checkKeyword(token::Keyword::Const)) {
            constTok = next().token;
        }

        token::Token specTok;
        if (checkKeyword(token::Keyword::Var)) {
            specTok = next().token;
        } else if (check(token::TokenKind::Keyword) && isTypeKeyword(peekToken(0).keyword)) {
            specTok = next().token;
        } else {
            errorHere("Expected type keyword or 'var' in variable declaration");
            specTok = next().token;
        }

        token::Token nameTok = expect(token::TokenKind::Identifier, "Expected identifier after type in variable declaration");
        if (had_error_) return nullptr;

        std::optional<token::Token> eq;
        ast::ExprPtr init;
        if (checkOperator(token::Operator::Assign)) {
            eq = next().token;
            init = parseExpression();
            if (had_error_) return nullptr;
        }

        token::Token semi = expectOperator(token::Operator::Semicolon, "Expected ';' after variable declaration");
        if (had_error_) return nullptr;

        auto stmt = std::make_unique<ast::Stmt>(ast::VarDeclStmt{
                .constToken = constTok,
                .specifierToken = specTok,
                .nameToken = nameTok,
                .equalsToken = eq,
                .initializer = std::move(init),
                .semicolon = semi,
        });
        return stmt;
    }

    ast::StmtPtr Parser::parseExprStatement() {
        auto expr = parseExpression();
        if (had_error_) return nullptr;
        token::Token semi = expectOperator(token::Operator::Semicolon, "Expected ';' after expression");
        if (had_error_) return nullptr;
        return std::make_unique<ast::Stmt>(ast::ExprStmt{.expr = std::move(expr), .semicolon = semi});
    }

    ast::ExprPtr Parser::parseExpression() {
        return parseExpressionPrec(static_cast<std::uint8_t>(Precedence::Assignment));
    }

    ast::ExprPtr Parser::parseExpressionPrec(std::uint8_t minPrec) {
        auto left = parsePrefix();
        if (!left) return nullptr;
        if (had_error_) return nullptr;

        while (true) {
            // Call has highest precedence; parse as postfix
            if (checkOperator(token::Operator::LeftParen)) {
                left = parsePostfix(std::move(left));
                if (had_error_) return nullptr;
                continue;
            }

            const auto &t = peekToken(0);
            if (t.kind != token::TokenKind::Operator) break;

            const Precedence prec = precedenceOf(t.op);
            const std::uint8_t p = static_cast<std::uint8_t>(prec);
            if (p < minPrec || prec == Precedence::None) break;

            token::Token opTok = next().token;
            const std::uint8_t nextMin = isRightAssociative(opTok.op) ? p : static_cast<std::uint8_t>(p + 1);
            auto right = parseExpressionPrec(nextMin);
            if (!right) return nullptr;
            if (had_error_) return nullptr;

            left = std::make_unique<ast::Expr>(ast::BinaryExpr{
                    .left = std::move(left),
                    .opToken = opTok,
                    .op = opTok.op,
                    .right = std::move(right),
            });
        }

        return left;
    }

    ast::ExprPtr Parser::parsePrefix() {
        if (checkOperator(token::Operator::Plus) || checkOperator(token::Operator::Minus) || checkOperator(token::Operator::Bang) ||
            checkOperator(token::Operator::Tilde)) {
            token::Token opTok = next().token;
            auto operand = parseExpressionPrec(static_cast<std::uint8_t>(Precedence::Prefix));
            if (!operand) return nullptr;
            if (had_error_) return nullptr;
            return std::make_unique<ast::Expr>(ast::UnaryExpr{
                    .opToken = opTok,
                    .op = opTok.op,
                    .operand = std::move(operand),
            });
        }

        auto prim = parsePrimary();
        if (!prim) return nullptr;
        if (had_error_) return nullptr;

        // Parse postfix calls repeatedly: f()(x)
        while (checkOperator(token::Operator::LeftParen)) {
            prim = parsePostfix(std::move(prim));
            if (had_error_) return nullptr;
        }

        return prim;
    }

    ast::ExprPtr Parser::parsePostfix(ast::ExprPtr left) {
        token::Token lparen = expectOperator(token::Operator::LeftParen, "Expected '('");
        if (had_error_) return nullptr;

        std::vector<ast::ExprPtr> args;
        if (!checkOperator(token::Operator::RightParen)) {
            while (true) {
                args.push_back(parseExpression());
                if (had_error_) return nullptr;
                if (checkOperator(token::Operator::Comma)) {
                    next();
                    continue;
                }
                break;
            }
        }

        token::Token rparen = expectOperator(token::Operator::RightParen, "Expected ')' after call arguments");
        if (had_error_) return nullptr;
        return std::make_unique<ast::Expr>(ast::CallExpr{
                .callee = std::move(left),
                .leftParen = lparen,
                .args = std::move(args),
                .rightParen = rparen,
        });
    }

    ast::ExprPtr Parser::parsePrimary() {
        if (check(token::TokenKind::Number)) {
            token::Token numTok = next().token;
            return std::make_unique<ast::Expr>(ast::NumberLiteralExpr{.token = numTok});
        }

        if (check(token::TokenKind::Identifier)) {
            token::Token idTok = next().token;
            return std::make_unique<ast::Expr>(ast::IdentifierExpr{.token = idTok});
        }

        if (checkOperator(token::Operator::LeftParen)) {
            token::Token lparen = next().token;
            auto inner = parseExpression();
            if (had_error_) return nullptr;
            token::Token rparen = expectOperator(token::Operator::RightParen, "Expected ')' after expression");
            if (had_error_) return nullptr;
            return std::make_unique<ast::Expr>(ast::ParenExpr{.leftParen = lparen, .expr = std::move(inner), .rightParen = rparen});
        }

        errorHere("Expected expression");
        return nullptr;
    }
}