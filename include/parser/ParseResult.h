#pragma once

#include <ast/Program.h>

#include <optional>

namespace cloth::parser {
    struct ParseResult {
        std::optional<ast::Program> program;
        bool had_error = false;
    };
}
