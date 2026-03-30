#include <lexer/Lexer.h>
#include <token/Token.h>
#include <exit/ExitCodes.h>

#include <iostream>
#include <string>
#include <vector>

#include "cmd/Command.h"
#include "file/SourceManager.h"

std::string source = R"(
module cloth;

import std.io;

public class MyClass(int number?) {
    const i32 myInt? { public get; };

    public MyClass {
        this.myInt = number;
    }

    public func convertIntToFloat() :> float {
        return getMyInt() as float;
    }
}
)";

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
    cloth::lexer::lexStream(lexer, false);

    std::printf("%s\n", source.c_str());

    return EXIT_SUCCESS;
}
