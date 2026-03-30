#include <iostream>
#include <lexer/Lexer.h>

#include <vector>

#include "token/TokenName.h"

namespace cloth::lexer {
    static constexpr bool kNestedBlockComments = true;

    Lexer::Lexer(const SourceBuffer &buffer, DiagnosticSink &diagnostics, LexerOptions options)
        : buffer_(&buffer), diagnostics_(diagnostics), options_(options) {
        begin_ = buffer_->text.data();
        current_ = begin_;
        end_ = begin_ + buffer_->text.size();

        location_.file = buffer_->file;
        location_.offset = 0;
        location_.line = 1;
        location_.column = 1;
    }

    // API
    const LexedToken &Lexer::peek(std::size_t n) {
        fillLookahead(n + 1);
        return la_[(la_head_ + n) % kLookahead];
    }

    LexedToken Lexer::next() {
        fillLookahead(1);
        LexedToken out = std::move(la_[la_head_]);
        la_head_ = (la_head_ + 1) % kLookahead;
        la_size_--;
        return out;
    }

    bool Lexer::eof() {
        return peek(0).token.kind == token::TokenKind::EndOfFile;
    }

    void Lexer::fillLookahead(std::size_t need) {
        while (la_size_ < need) {
            if (la_size_ >= kLookahead) {
                diagnostics_.error(location_, "Buffer overflow in lexer lookahead.");
                break;
            }

            const std::size_t tail = (la_head_ + la_size_) % kLookahead;
            la_[tail] = lexOne();
            la_size_++;
        }
    }

    LexedToken Lexer::lexOne() {
        LexedToken out{};
        out.trivia.clear();

        // Leading trivia (whitespace/comments) before the token
        if (options_.keep_trivia || !options_.emit_whitespace || !options_.emit_comments) {
            consumeTrivia(out.trivia.leading);
        }

        beginToken();

        if (atEnd()) {
            auto eofTok = scanEndOfFile();
            // Trailing trivia after EOF is not meaningful; usually none anyway.
            return emit(std::move(eofTok));
        }

        const char c = current();

        // Optional: emit whitespace tokens rather than consuming as trivia
        if (options_.emit_whitespace && isWhitespace(c)) {
            auto ws = scanWhitespaceToken();
            maybeConsumeTrailingTrivia(ws.trivia);
            return emit(std::move(ws));
        }

        // Ident / keyword
        if (isIdentStart(c)) {
            auto id = scanIdentifierOrKeyword();
            maybeConsumeTrailingTrivia(id.trivia);
            return emit(std::move(id));
        }

        // Number (starting with digit; handle .123 in operator scanner if you allow)
        if (isDigit(c)) {
            auto num = scanNumber();
            maybeConsumeTrailingTrivia(num.trivia);
            return emit(std::move(num));
        }

        // String
        if (c == '"' || c == '\'') {
            auto str = scanStringLiteral(c);
            maybeConsumeTrailingTrivia(str.trivia);
            return emit(std::move(str));
        }

        // Slash: comment or operator
        if (c == '/') {
            auto slash = scanCommentOrSlashOperator();
            // If comments are tokens, trailing trivia should still attach.
            maybeConsumeTrailingTrivia(slash.trivia);
            return emit(std::move(slash));
        }

        // Operator / punctuation / unknown
        auto op = scanOperatorOrPunctuation();
        maybeConsumeTrailingTrivia(op.trivia);
        return emit(std::move(op));
    }

    LexedToken Lexer::emit(LexedToken t) const {
        if (!options_.keep_trivia) {
            t.trivia.clear();
        }
        return t;
    }

    // --- Cursor Primitives ---
    void Lexer::advance() noexcept {
        if (atEnd()) return;
        const char consumed = *current_++;
        bumpLocation(consumed);
    }

    void Lexer::advanceN(std::size_t n) noexcept {
        while (n-- && !atEnd()) advance();
    }

    bool Lexer::match(char c) noexcept {
        if (current() != c) return false;
        advance();
        return true;
    }

    bool Lexer::match2(char a, char b) noexcept {
        if (current() == a && lookaheadChar(1) == b) {
            advanceN(2);
            return true;
        }
        return false;
    }

    bool Lexer::matchString(std::string_view s) noexcept {
        if (s.empty()) return true;
        const std::size_t n = s.size();
        if (static_cast<std::size_t>(end_ - current_) < n) return false;
        for (std::size_t i = 0; i < n; ++i) {
            if (current_[i] != s[i]) return false;
        }
        advanceN(n);
        return true;
    }

    // --- Token Lifecycle ---
    void Lexer::beginToken() noexcept {
        token_begin_ = current_;
        token_location_ = location_;
    }

    token::SourceSpan Lexer::endTokenSpan() const noexcept {
        token::SourceLocation endLoc = location_;
        endLoc.offset = offsetFromBegin(current_);
        return token::SourceSpan{token_location_, endLoc};
    }

    std::string_view Lexer::tokenLexeme() const noexcept {
        return {token_begin_, static_cast<std::size_t>(current_ - token_begin_)};
    }

    token::Token Lexer::makeToken(token::TokenKind kind) const {
        token::Token t;
        t.kind = kind;
        t.span = endTokenSpan();
        t.lexeme = tokenLexeme();
        return t;
    }

    token::Token Lexer::makeToken(token::TokenKind kind, token::Keyword kw) const {
        token::Token t = makeToken(kind);
        t.keyword = kw;
        return t;
    }

    token::Token Lexer::makeToken(token::TokenKind kind, token::Operator op) const {
        token::Token t = makeToken(kind);
        t.op = op;
        return t;
    }

    token::Token Lexer::makeErrorToken(std::string_view message) const {
        // error tokens should still carry a span/lexeme (even if empty) for tooling
        diagnostics_.error(token_location_, message);
        token::Token t = makeToken(token::TokenKind::Error);
        return t;
    }

    meta_token::MetaToken Lexer::makeMetaToken(token::TokenKind kind) const {
        meta_token::MetaToken t;
        t.kind = kind;
        t.span = endTokenSpan();
        t.lexeme = tokenLexeme();
        return t;
    }

    // --- Classification ---
    bool Lexer::isWhitespace(char c) noexcept {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    }

    bool Lexer::isNewline(char c) noexcept {
        return c == '\n';
    }

    bool Lexer::isDigit(char c) noexcept {
        return c >= '0' && c <= '9';
    }

    bool Lexer::isHexDigit(char c) noexcept {
        return (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    }

    bool Lexer::isAlpha(char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    // TODO: Unicode identifier support (e.g. isIdentStart can include non-ASCII letters, isIdentContinue can include combining marks, etc.)
    // TODO: We can basically make any rules we want for identifiers, as long as the lexer and parser are consistent. For example, we could allow emojis in identifiers if we wanted to. The current rules are just a common baseline.
    bool Lexer::isIdentStart(char c) noexcept {
        return isAlpha(c) || c == '_' || c == '$';
    }

    bool Lexer::isIdentContinue(char c) noexcept {
        return isIdentStart(c) || isDigit(c);
    }

    // --- Trivia ---
    void Lexer::consumeTrivia(std::vector<TriviaPiece> &outLeading) {
        // Collect whitespace/comments as trivia, skipping them from token lexing.
        // If emit_whitespace/comments are true, caller won’t route here for whitespace;
        // comments still can be filtered here depending on opts.
        while (!atEnd()) {
            const char c = current();

            // Whitespace
            if (isWhitespace(c)) {
                // If whitespace is supposed to be a token, stop here.
                if (options_.emit_whitespace) break;

                // Otherwise consume as trivia
                beginToken();
                while (!atEnd() && isWhitespace(current())) advance();

                if (options_.keep_trivia) {
                    TriviaPiece tr;
                    tr.kind = token::TokenKind::Whitespace;
                    tr.span = endTokenSpan();
                    tr.lexeme = tokenLexeme();
                    outLeading.push_back(tr);
                }
                continue;
            }

            // Comments
            if (c == '/' && (lookaheadChar(1) == '/' || lookaheadChar(1) == '*')) {
                // If comment should be a token, stop here.
                if (options_.emit_comments) break;

                beginToken();
                bool ok = (lookaheadChar(1) == '/') ? consumeLineComment() : consumeBlockComment();
                (void) ok;

                if (options_.keep_trivia) {
                    TriviaPiece tr;
                    tr.kind = token::TokenKind::Comment;
                    tr.span = endTokenSpan();
                    tr.lexeme = tokenLexeme();
                    outLeading.push_back(tr);
                }
                continue;
            }

            break;
        }
    }

    void Lexer::maybeConsumeTrailingTrivia(Trivia &outTrailing) {
        if (!options_.keep_trivia) return;

        // Common approach: only take trailing trivia on the same line.
        // That keeps “// comment” attached to the statement token.
        // We’ll implement:
        //   - consume spaces/tabs
        //   - consume line comment
        //   - stop at the newline or EOF
        while (!atEnd()) {
            const char c = current();
            if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
                beginToken();
                while (!atEnd()) {
                    char cc = current();
                    if (cc == ' ' || cc == '\t' || cc == '\v' || cc == '\f') advance();
                    else break;
                }
                TriviaPiece tr{token::TokenKind::Whitespace, endTokenSpan(), tokenLexeme()};
                outTrailing.trailing.push_back(tr);
                continue;
            }

            if (c == '/' && lookaheadChar(1) == '/') {
                beginToken();
                consumeLineComment();
                TriviaPiece tr{token::TokenKind::Comment, endTokenSpan(), tokenLexeme()};
                outTrailing.trailing.push_back(tr);
                continue;
            }

            // Stop at newline or anything else
            break;
        }
    }

    // --- Scanners ---
    LexedToken Lexer::scanEndOfFile() {
        beginToken();
        // don’t advance; eof lexeme is empty
        LexedToken t;
        t.token = makeToken(token::TokenKind::EndOfFile);
        return t;
    }

    LexedToken Lexer::scanWhitespaceToken() {
        // Only called when emit_whitespace=true and current is whitespace
        beginToken();
        while (!atEnd() && isWhitespace(current())) advance();

        LexedToken t;
        t.token = makeToken(token::TokenKind::Whitespace);
        return t;
    }

    LexedToken Lexer::scanIdentifierOrKeyword() {
        beginToken();
        // ASCII mode for now
        advance(); // first char
        while (!atEnd() && isIdentContinue(current())) advance();

        const std::string_view ident = tokenLexeme();

        // Check for meta-keywords (UPPERCASE identifiers like MAX, SIZEOF, etc.)
        // These will be used in expressions like: i32::MAX, String::LENGTH
        const meta_token::MetaKeyword metaKw = resolveMetaToken(ident);
        if (metaKw != meta_token::MetaKeyword::None) {
            LexedToken t;
            // Mark this as a Meta token kind so parser knows it's a meta keyword
            t.token = makeToken(token::TokenKind::Meta);
            // Store which meta keyword it is in the flags field (temporary solution)
            // Or we could store it differently - the parser will need to check the lexeme anyway
            return t;
        }

        // Then check for regular keywords
        const token::Keyword kw = resolveKeyword(ident);

        LexedToken t;
        if (kw != token::Keyword::None) t.token = makeToken(token::TokenKind::Keyword, kw);
        else t.token = makeToken(token::TokenKind::Identifier);
        return t;
    }

    LexedToken Lexer::scanNumber() {
        beginToken();

        // integers, floats, bases, separators, suffixes
        // This is structured so you can extend without rewriting.
        // Rules sketch:
        //  - 0x... hex
        //  - 0b... binary
        //  - digits with optional '_' separators
        //  - optional fraction '.' digits
        //  - optional exponent e/E (+/-) digits
        //  - optional suffix (u32, i64, f32, etc.) (decide for Cloth)

        // Base prefixes
        if (current() == '0' && (lookaheadChar(1) == 'x' || lookaheadChar(1) == 'X')) {
            advanceN(2);
            bool any = false;
            while (!atEnd()) {
                char c = current();
                if (c == '_') {
                    advance();
                    continue;
                }
                if (!isHexDigit(c)) break;
                any = true;
                advance();
            }
            if (!any) {
                LexedToken t;
                t.token = makeErrorToken("Malformed hex literal (expected digits after 0x)");
                return t;
            }
            // TODO suffix parsing
            LexedToken t;
            t.token = makeToken(token::TokenKind::Number);
            return t;
        }

        if (current() == '0' && (lookaheadChar(1) == 'b' || lookaheadChar(1) == 'B')) {
            advanceN(2);
            bool any = false;
            while (!atEnd()) {
                char c = current();
                if (c == '_') {
                    advance();
                    continue;
                }
                if (c != '0' && c != '1') break;
                any = true;
                advance();
            }
            if (!any) {
                LexedToken t;
                t.token = makeErrorToken("Malformed binary literal (expected digits after 0b)");
                return t;
            }
            LexedToken t;
            t.token = makeToken(token::TokenKind::Number);
            return t;
        }

        // Decimal integer part
        while (!atEnd()) {
            char c = current();
            if (c == '_') {
                advance();
                continue;
            }
            if (!isDigit(c)) break;
            advance();
        }

        // Fractional part
        bool isFloat = false;
        if (current() == '.' && isDigit(lookaheadChar(1))) {
            isFloat = true;
            advance(); // '.'
            while (!atEnd()) {
                char c = current();
                if (c == '_') {
                    advance();
                    continue;
                }
                if (!isDigit(c)) break;
                advance();
            }
        }

        // Exponent
        if (current() == 'e' || current() == 'E') {
            isFloat = true;
            advance();
            if (current() == '+' || current() == '-') advance();

            bool any = false;
            while (!atEnd()) {
                char c = current();
                if (c == '_') {
                    advance();
                    continue;
                }
                if (!isDigit(c)) break;
                any = true;
                advance();
            }
            if (!any) {
                LexedToken t;
                t.token = makeErrorToken("Malformed exponent in numeric literal");
                return t;
            }
        }

        // Optional suffix (example: u32, i64, f32)
        // Keep it simple: suffix starts with alpha.
        if (isAlpha(current())) {
            // consume suffix chars [a-zA-Z0-9]
            while (!atEnd()) {
                char c = current();
                if (isAlpha(c) || isDigit(c)) advance();
                else break;
            }
        }

        LexedToken t;
        t.token = makeToken(token::TokenKind::Number);
        (void) isFloat; // you can encode in Token.flags or parse later
        return t;
    }

    LexedToken Lexer::scanStringLiteral(char quote) {
        beginToken();

        // Opening quote
        advance();

        std::uint32_t bytes = 0;
        while (!atEnd()) {
            char c = current();

            // Closing quote
            if (c == quote) {
                advance();
                LexedToken t;
                t.token = makeToken(token::TokenKind::String);
                return t;
            }

            // Newline in string (if not allowed)
            if (c == '\n') {
                LexedToken t;
                t.token = makeErrorToken("Unterminated string literal");
                return t;
            }

            // Escape
            if (c == '\\') {
                advance();
                if (!consumeEscapeSequence()) {
                    LexedToken t;
                    t.token = makeErrorToken("Invalid escape sequence in string literal");
                    return t;
                }
                bytes += 1;
            } else {
                advance();
                bytes += 1;
            }

            if (bytes > options_.max_string_literal_bytes) {
                LexedToken t;
                t.token = makeErrorToken("String literal exceeds maximum size limit");
                return t;
            }
        }

        LexedToken t;
        t.token = makeErrorToken("Unterminated string literal at end of file");
        return t;
    }

    LexedToken Lexer::scanCommentOrSlashOperator() {
        // current == '/'
        if (lookaheadChar(1) == '/') {
            if (options_.emit_comments) {
                beginToken();
                consumeLineComment();
                LexedToken t;
                t.token = makeToken(token::TokenKind::Comment);
                return t;
            }

            // otherwise consume as trivia in consumeTrivia() earlier,
            // but if we get here, treat it as skipped comment and then lex again
            Trivia dummy;
            consumeTrivia(dummy.leading);
            return lexOne();
        }

        if (lookaheadChar(1) == '*') {
            if (options_.emit_comments) {
                beginToken();
                consumeBlockComment();
                LexedToken t;
                t.token = makeToken(token::TokenKind::Comment);
                return t;
            }

            Trivia dummy;
            consumeTrivia(dummy.leading);
            return lexOne();
        }

        // It's an operator '/'
        beginToken();
        advance();
        LexedToken t;
        t.token = makeToken(token::TokenKind::Operator, token::Operator::Slash);
        return t;
    }

    LexedToken Lexer::scanOperatorOrPunctuation() {
        beginToken();

        // 3-char operators
        if (matchString(">>=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, resolveOperator(">>="));
            return t;
        }
        if (matchString("<<=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, resolveOperator("<<="));
            return t;
        }
        if (matchString("...")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::DotDotDot);
            return t;
        }

        // 2-char operators/punctuation
        if (matchString("++")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::PlusPlus);
            return t;
        }
        if (matchString("--")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::MinusMinus);
            return t;
        }
        if (matchString("==")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::Equal);
            return t;
        }
        if (matchString("!=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::NotEqual);
            return t;
        }
        if (matchString("<=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::LessEqual);
            return t;
        }
        if (matchString(">=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::GreaterEqual);
            return t;
        }
        if (matchString("->")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::Arrow);
            return t;
        }
        if (matchString("+=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::PlusAssign);
            return t;
        }
        if (matchString("-=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::MinusAssign);
            return t;
        }
        if (matchString("*=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::StarAssign);
            return t;
        }
        if (matchString("/=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::SlashAssign);
            return t;
        }
        if (matchString("%=")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::PercentAssign);
            return t;
        }
        if (matchString("::")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::ColonColon);
            return t;
        }
        if (matchString("..")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::DotDot);
            return t;
        }
        if (matchString(":>")) {
            LexedToken t;
            t.token = makeToken(token::TokenKind::Operator, token::Operator::ReturnArrow);
            return t;
        }
        // Add ::, .., ..., ?? etc as needed.

        // 1-char tokens
        const char c = current();
        advance();

        switch (c) {
            case '+': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Plus);
                return t;
            }
            case '-': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Minus);
                return t;
            }
            case '*': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Star);
                return t;
            }
            case '/': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Slash);
                return t;
            }
            case '%': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Percent);
                return t;
            }
            case '=': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Assign);
                return t;
            }
            case '<': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Less);
                return t;
            }
            case '>': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Greater);
                return t;
            }
            case '!': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Bang);
                return t;
            }
            case '&': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Amp);
                return t;
            }
            case '|': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Pipe);
                return t;
            }
            case '~': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Tilde);
                return t;
            }

            case '(': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::LeftParen);
                return t;
            }
            case ')': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::RightParen);
                return t;
            }
            case '{': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::LeftBrace);
                return t;
            }
            case '}': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::RightBrace);
                return t;
            }
            case '[': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::LeftBracket);
                return t;
            }
            case ']': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::RightBracket);
                return t;
            }

            case '.': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::Dot);
                return t;
            }
            case ',': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::Comma);
                return t;
            }
            case ':': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::Colon);
                return t;
            }
            case ';': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Punctuation, token::Operator::Semicolon);
                return t;
            }
            case '?': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Question);
                return t;
            }
            case '@': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::At);
                return t;
            }
            case '#': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Hash);
                return t;
            }
            case '$': {
                LexedToken t;
                t.token = makeToken(token::TokenKind::Operator, token::Operator::Dollar);
                return t;
            }

            default: break;
        }

        // Unknown character (still consume it so we don’t infinite-loop)
        LexedToken t;
        t.token = makeErrorToken("Unexpected character in input");
        return t;
    }

    // --- Comments and String helpers ---
    bool Lexer::consumeLineComment() {
        // assumes current is '/' and next is '/'
        if (!matchString("//")) return false;
        while (!atEnd() && current() != '\n') advance();
        return true;
    }

    bool Lexer::consumeBlockComment() {
        // assumes current is '/' and next is '*'
        if (!matchString("/*")) return false;

        int depth = 1;
        while (!atEnd()) {
            if (matchString("*/")) {
                depth--;
                if (depth == 0) return true;
                continue;
            }

            if constexpr (kNestedBlockComments) {
                if (matchString("/*")) {
                    depth++;
                    continue;
                }
            }

            advance();
        }

        // If we hit EOF without closing
        diagnostics_.error(token_location_, "Unterminated block comment");
        return false;
    }

    bool Lexer::consumeEscapeSequence() {
        // Called after consuming backslash '\'
        if (atEnd()) return false;
        char c = current();
        advance();

        switch (c) {
            case 'n':
            case 'r':
            case 't':
            case '\\':
            case '\'':
            case '"':
            case '0':
                return true;
            case 'x': {
                // \xHH
                if (!isHexDigit(current())) return false;
                advance();
                if (!isHexDigit(current())) return false;
                advance();
                return true;
            }
            case 'u': {
                // \u{...} or \uFFFF (choose)
                // Placeholder: accept \uFFFF exactly
                for (int i = 0; i < 4; ++i) {
                    if (!isHexDigit(current())) return false;
                    advance();
                }
                return true;
            }
            default:
                return false;
        }
    }

    // --- Keyword and Operator Resolution ---
    token::Keyword Lexer::resolveKeyword(std::string_view ident) noexcept {
        switch (!ident.empty() ? ident[0] : '\0') {
            case 'a': {
                if (ident == "as") return token::Keyword::As;
                if (ident == "async") return token::Keyword::Async;
                if (ident == "await") return token::Keyword::Await;
                if (ident == "atomic") return token::Keyword::Atomic;
                if (ident == "and") return token::Keyword::And;
                if (ident == "any") return token::Keyword::Any;
                break;
            }
            case 'b': {
                if (ident == "break") return token::Keyword::Break;
                if (ident == "bool") return token::Keyword::Bool;
                if (ident == "byte") return token::Keyword::Byte;
                if (ident == "bit") return token::Keyword::Bit;
                break;
            }
            case 'c': {
                if (ident == "const") return token::Keyword::Const;
                if (ident == "continue") return token::Keyword::Continue;
                if (ident == "class") return token::Keyword::Class;
                if (ident == "case") return token::Keyword::Case;
                if (ident == "char") return token::Keyword::Char;
                if (ident == "catch") return token::Keyword::Catch;
                break;
            }
            case 'd': {
                if (ident == "defer") return token::Keyword::Defer;
                if (ident == "delete") return token::Keyword::Delete;
                if (ident == "do") return token::Keyword::Do;
                if (ident == "default") return token::Keyword::Default;
                if (ident == "double") return token::Keyword::F64; // double -> F64 mapping
                break;
            }
            case 'e': {
                if (ident == "else") return token::Keyword::Else;
                if (ident == "enum") return token::Keyword::Enum;
                break;
            }
            case 'f': {
                if (ident == "for") return token::Keyword::For;
                if (ident == "func") return token::Keyword::Func;
                if (ident == "float") return token::Keyword::F32;
                if (ident == "f32") return token::Keyword::F32;
                if (ident == "f64") return token::Keyword::F64;
                if (ident == "finally") return token::Keyword::Finally;
                if (ident == "false") return token::Keyword::False;
                break;
            }
            case 'g': break;
                if (ident == "get") return token::Keyword::Get;
            case 'h': break;
            case 'i': {
                if (ident == "interface") return token::Keyword::Interface;
                if (ident == "internal") return token::Keyword::Internal;
                if (ident == "import") return token::Keyword::Import;
                if (ident == "int") return token::Keyword::I32; // int -> I32 mapping
                if (ident == "i8") return token::Keyword::I8;
                if (ident == "i16") return token::Keyword::I16;
                if (ident == "i32") return token::Keyword::I32;
                if (ident == "i64") return token::Keyword::I64;
                if (ident == "if") return token::Keyword::If;
                if (ident == "in") return token::Keyword::In;
                if (ident == "is") return token::Keyword::Is;
                break;
            }
            case 'j': break;
            case 'k': break;
            case 'l': {
                if (ident == "let") return token::Keyword::Let;
                if (ident == "long") return token::Keyword::I64; // long -> I64 mapping
                break;
            }
            case 'm': {
                if (ident == "module") return token::Keyword::Module;
                break;
            }
            case 'n': {
                if (ident == "null") return token::Keyword::Null;
                if (ident == "new") return token::Keyword::New;
                break;
            }
            case 'o': {
                if (ident == "or") return token::Keyword::Or;
                if (ident == "owned") return token::Keyword::Owned;
                break;
            }
            case 'p': {
                if (ident == "public") return token::Keyword::Public;
                if (ident == "private") return token::Keyword::Private;
                break;
            }
            case 'q': break;
            case 'r': {
                if (ident == "return") return token::Keyword::Return;
                if (ident == "real") return token::Keyword::F64; // real -> F64 mapping
                break;
            }
            case 's': {
                if (ident == "struct") return token::Keyword::Struct;
                if (ident == "switch") return token::Keyword::Switch;
                if (ident == "string") return token::Keyword::String;
                if (ident == "super") return token::Keyword::Super;
                if (ident == "short") return token::Keyword::I16; // short -> I16 mapping
                if (ident == "shared") return token::Keyword::Shared;
                if (ident == "static") return token::Keyword::Static;
                if (ident == "set") return token::Keyword::Set;
                break;
            }
            case 't': {
                if (ident == "this") return token::Keyword::This;
                if (ident == "throw") return token::Keyword::Throw;
                if (ident == "try") return token::Keyword::Try;
                if (ident == "true") return token::Keyword::True;
                break;
            }
            case 'u': {
                if (ident == "uint") return token::Keyword::U32;
                if (ident == "u8") return token::Keyword::U8;
                if (ident == "u16") return token::Keyword::U16;
                if (ident == "u32") return token::Keyword::U32;
                if (ident == "u64") return token::Keyword::U64;
                break;
            }
            case 'v': {
                if (ident == "var") return token::Keyword::Var;
                if (ident == "void") return token::Keyword::Void;
                break;
            }
            case 'w': {
                if (ident == "while") return token::Keyword::While;
                break;
            }
            case 'x': break;
            case 'y': break;
            case 'z': break;
            default: break;
        }
        return token::Keyword::None;
    }

    token::Operator Lexer::resolveOperator(std::string_view opText) noexcept {
        // Only used for multi-char operators you want centralized
        if (opText == ">>=") return token::Operator::None; // fill as needed
        if (opText == "<<=") return token::Operator::None;
        return token::Operator::None;
    }

    meta_token::MetaKeyword Lexer::resolveMetaToken(std::string_view ident) noexcept {
        switch (!ident.empty() ? ident[0] : '\0') {
            case 'A': {
                if (ident == "ALIGNOF") return meta_token::MetaKeyword::ALIGNOF;
                break;
            }
            case 'D': {
                if (ident == "DEFAULT") return meta_token::MetaKeyword::DEFAULT;
                break;
            }
            case 'L': {
                if (ident == "LENGTH") return meta_token::MetaKeyword::LENGTH;
                break;
            }
            case 'M': {
                if (ident == "MEMSPACE") return meta_token::MetaKeyword::MEMSPACE;
                if (ident == "MAX") return meta_token::MetaKeyword::MAX;
                if (ident == "MIN") return meta_token::MetaKeyword::MIN;
                break;
            }
            case 'S': {
                if (ident == "SIZEOF") return meta_token::MetaKeyword::SIZEOF;
                break;
            }
            case 'T': {
                if (ident == "TO_STRING") return meta_token::MetaKeyword::TO_STRING;
                if (ident == "TYPEOF") return meta_token::MetaKeyword::TYPEOF;
                if (ident == "TO_BYTES") return meta_token::MetaKeyword::TO_BYTES;
                if (ident == "TO_BITS") return meta_token::MetaKeyword::TO_BITS;
                break;
            }
            default: break;
        }
        return meta_token::MetaKeyword::None;
    }

    // --- Location Tracking ---
    void Lexer::bumpLocation(char consumed) noexcept {
        location_.offset = offsetFromBegin(current_);

        if (consumed == '\n') {
            location_.line += 1;
            location_.column = 1;
        } else {
            location_.column += 1;
        }
    }

    static void printToken(const LexedToken& lt) {
        const auto& t = lt.token;

        std::cout << "----------------------------------------\n";
        std::cout << "Kind: " << debug::to_string(t.kind) << "\n";

        // Keyword / operator info (if your Token carries these)
        if (t.kind == token::TokenKind::Keyword) {
            std::cout << "Keyword: " << static_cast<std::uint32_t>(t.keyword) << "\n";
        }
        else if (t.kind == token::TokenKind::Operator || t.kind == token::TokenKind::Punctuation) {
            std::cout << "Operator/Punctuation: " << static_cast<std::uint32_t>(t.op) << "\n";
        }

        std::cout << "Lexeme: `" << t.lexeme << "`\n";

        std::cout
            << "Span: "
            << "file=" << t.span.begin.file
            << " [" << t.span.begin.line << ":" << t.span.begin.column << " off=" << t.span.begin.offset << "]"
            << " .. "
            << "file=" << t.span.end.file
            << " [" << t.span.end.line << ":" << t.span.end.column << " off=" << t.span.end.offset << "]"
            << " len=" << t.span.length()
            << "\n";

        if (!lt.trivia.leading.empty()) {
            std::cout << "Leading trivia:\n";
            for (const auto& tr : lt.trivia.leading) {
                std::cout << "  - " << debug::to_string(tr.kind) << " `" << tr.lexeme << "`\n";
            }
        }
        if (!lt.trivia.trailing.empty()) {
            std::cout << "Trailing trivia:\n";
            for (const auto& tr : lt.trivia.trailing) {
                std::cout << "  - " << debug::to_string(tr.kind) << " `" << tr.lexeme << "`\n";
            }
        }
    }

    void lexStream(Lexer &lexer, bool printStream) {
        while (true) {
            LexedToken tok = lexer.next();
            if (printStream) printToken(tok);

            if (tok.token.kind == token::TokenKind::EndOfFile) {
                break;
            }
        }
    }
} // namespace cloth::lexer
