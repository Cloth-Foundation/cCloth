#pragma once

#include <ast/Stmt.h>

#include <vector>

namespace cloth::ast {
    struct Program {
        std::vector<StmtPtr> statements;
    };
}
