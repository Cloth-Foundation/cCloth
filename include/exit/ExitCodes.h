#ifndef CCLOTH_EXITCODES_H
#define CCLOTH_EXITCODES_H

#pragma once

#define EXIT_SUCCESS         0  // success
#define EXIT_FAILURE         1  // generic failure

#define EXIT_INVALID_USAGE   2  // bad arguments / CLI misuse
#define EXIT_CANNOT_EXEC     126 // found but not executable (shell convention)
#define EXIT_NOT_FOUND       127 // command is not found (shell convention)

#define EXIT_SIGNAL_BASE     128 // + signal number (Unix convention)

namespace cloth::exit {
    struct ExitInfo {
        int code;
        const char *message;
    };
}

#endif //CCLOTH_EXITCODES_H
