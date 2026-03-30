#pragma once
#include <lexer/Lexer.h>

#include <string>
#include <string_view>

namespace cloth::file {
    class SourceFile {
    public:
        SourceFile(token::FileId file, std::string path, std::string text)
            : path_(std::move(path)), buffer_{file, std::move(text), path_} {
        }

        [[nodiscard]] std::string_view getPath() const noexcept {
            return path_;
        }

        [[nodiscard]] std::string_view getName() const noexcept {
            return buffer_.filename;
        }

        [[nodiscard]] const lexer::SourceBuffer &buffer() const noexcept {
            return buffer_;
        }

    private:
        std::string path_;
        lexer::SourceBuffer buffer_;
    };
}
