#include <lexer/Lexer.h>
#include <token/Token.h>
#include <exit/ExitCodes.h>
#include <exit/Error.h>

#include <parser/Parser.h>

#include <iostream>
#include <string>
#include <vector>

#include "cmd/Command.h"
#include "file/SourceManager.h"

std::string source = R"(
i64 x = 5;
print(x);
)";

struct StdErrDiagnostics final : cloth::lexer::DiagnosticSink {
    void error(cloth::token::SourceLocation loc, std::string_view message) override {
        cloth::error::println(
                cloth::error::ErrorType::SYNTAX_ERROR,
                std::string(" file=") + std::to_string(loc.file) +
                        " line=" + std::to_string(loc.line) +
                        " col=" + std::to_string(loc.column) +
                        " off=" + std::to_string(loc.offset) +
                        ": " + std::string(message) + "\n");
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

struct ParserStdErrDiagnostics final : cloth::parser::DiagnosticSink {
    void error(cloth::token::SourceLocation loc, std::string_view message) override {
        cloth::error::println(
                cloth::error::ErrorType::PARSING_ERROR,
                std::string(" file=") + std::to_string(loc.file) +
                        " line=" + std::to_string(loc.line) +
                        " col=" + std::to_string(loc.column) +
                        " off=" + std::to_string(loc.offset) +
                        ": " + std::string(message) + "\n");
    }
};

int main(int argc, char **argv) {
    // Build the buffer (the std::string must outlive the lexer!)
    cloth::cmd::parse(argc, argv);

    cloth::file::SourceManager sources;
    const cloth::token::FileId file = sources.addVirtualFile("example.co", source);
    const cloth::lexer::SourceBuffer &buffer = sources.getBuffer(file);

    StdErrDiagnostics diags;

    cloth::lexer::LexerOptions opts;
    opts.emit_whitespace = false;
    opts.emit_comments = false;
    opts.keep_trivia = false;

    cloth::lexer::Lexer lexer(buffer, diags, opts);

    ParserStdErrDiagnostics pdiags;
    cloth::parser::Parser parser(lexer, pdiags);
    const auto result = parser.parseProgram();

    if (result.had_error) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
