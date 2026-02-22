#include "Lexer.hpp"
#include "Token.hpp"
#include "TokenName.hpp"

#include <iostream>
#include <string>
#include <vector>

struct StdErrDiagnostics final : cloth::lexer::DiagnosticSink {
    void error(cloth::token::SourceLocation loc, std::string_view message) override {
        std::cerr
            << "[error] file=" << loc.file
            << " line=" << loc.line
            << " col=" << loc.column
            << " off=" << loc.offset
            << ": " << message << "\n";
    }

    void warning(cloth::token::SourceLocation loc, std::string_view message) override {
        std::cerr
            << "[warn ] file=" << loc.file
            << " line=" << loc.line
            << " col=" << loc.column
            << " off=" << loc.offset
            << ": " << message << "\n";
    }
};

static void printToken(const cloth::lexer::LexedToken& lt) {
    const auto& t = lt.token;

    std::cout << "----------------------------------------\n";
    std::cout << "Kind: " << cloth::debug::to_string(t.kind) << "\n";

    // Keyword / operator info (if your Token carries these)
    if (t.kind == cloth::token::TokenKind::Keyword) {
        std::cout << "Keyword: " << static_cast<std::uint32_t>(t.keyword) << "\n";
    }
    else if (t.kind == cloth::token::TokenKind::Operator || t.kind == cloth::token::TokenKind::Punctuation) {
        std::cout << "Operator/Punct: " << static_cast<std::uint32_t>(t.op) << "\n";
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
            std::cout << "  - " << cloth::debug::to_string(tr.kind) << " `" << tr.lexeme << "`\n";
        }
    }
    if (!lt.trivia.trailing.empty()) {
        std::cout << "Trailing trivia:\n";
        for (const auto& tr : lt.trivia.trailing) {
            std::cout << "  - " << cloth::debug::to_string(tr.kind) << " `" << tr.lexeme << "`\n";
        }
    }
}

int main() {
    // Example source text (replace with file input later)
    std::string source = R"(
        const x: int = 10;
        if (x is i32) print(true);
    )";

    // Build the buffer (the std::string must outlive the lexer!)
    cloth::lexer::SourceBuffer buffer;
    buffer.file = 1;
    buffer.text = std::string_view(source);
    buffer.filename = "example.co";

    StdErrDiagnostics diags;

    cloth::lexer::LexerOptions opts;
    opts.emit_whitespace = false;
    opts.emit_comments = false;
    opts.keep_trivia = false;

    cloth::lexer::Lexer lexer(buffer, diags, opts);

    // Drive the lexer
    while (true) {
        cloth::lexer::LexedToken tok = lexer.next();
        printToken(tok);

        if (tok.token.kind == cloth::token::TokenKind::EndOfFile) {
            break;
        }
    }

    std::printf(source.c_str());

    return 0;
}