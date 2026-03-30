#pragma once

#include <file/SourceFile.h>

#include <token/Token.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cloth::file {
    class SourceManager {
    public:
        SourceManager() = default;

        [[nodiscard]] token::FileId addVirtualFile(std::string path, std::string text) {
            const token::FileId id = next_id_++;
            files_.emplace_back(id, std::move(path), std::move(text));
            return id;
        }

        [[nodiscard]] const SourceFile &getFile(token::FileId id) const {
            return files_.at(indexFromId(id));
        }

        [[nodiscard]] const lexer::SourceBuffer &getBuffer(token::FileId id) const {
            return getFile(id).buffer();
        }

        [[nodiscard]] const SourceFile &getFileByPath(std::string_view path) const {
            const auto it = path_to_id_.find(std::string(path));
            return getFile(it->second);
        }

        [[nodiscard]] token::FileId addFile(std::string path, std::string text) {
            const token::FileId id = addVirtualFile(path, std::move(text));
            path_to_id_.emplace(std::move(path), id);
            return id;
        }

    private:
        [[nodiscard]] static std::size_t indexFromId(token::FileId id) {
            return id - 1;
        }

        token::FileId next_id_ = 1;
        std::vector<SourceFile> files_;
        std::unordered_map<std::string, token::FileId> path_to_id_;
    };
} // namespace cloth::file
