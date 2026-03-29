#pragma once

#include <iostream>
#include <string>

namespace cloth::error {
    enum class ErrorType : int {
        SYNTAX_ERROR = 0,
        COMPILER_ERROR,
        PARSING_ERROR,
    };

    static std::string getErrorString(int i) {
        switch (i) {
            case 0: return "Syntax Error";
            case 1: return "Compiler Error";
            case 2: return "Parsing Error";
            default: return "Unknown Error";
        }
    }

    static std::string toString(ErrorType error) {
        switch (error) {
            case ErrorType::SYNTAX_ERROR: return std::string("Syntax Error");
                break;
            case ErrorType::COMPILER_ERROR: return std::string("Compiler Error");
                break;
            case ErrorType::PARSING_ERROR: return std::string("Parsing Error");
                break;
            default: return std::string("Unknown Error");
                break;
        }
    }

    static void println(ErrorType error, std::string message) {
        std::printf("[%s] %s", toString(error).c_str(), message.c_str());
    }

    static void println(ErrorType error) {
        println(error, getErrorString(static_cast<int>(error)));
    }
}
