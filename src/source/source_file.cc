#include "cloth/source/source_file.h"

#include <fstream>
#include <iterator>
#include <utility>

namespace cloth {

struct SourceFile::Storage {
  std::filesystem::path path;
  std::string display_path;
  std::string contents;
};

SourceFile::SourceFile(std::unique_ptr<Storage> storage) noexcept
    : storage_(std::move(storage)) {}

SourceFile::SourceFile(SourceFile&&) noexcept = default;
SourceFile& SourceFile::operator=(SourceFile&&) noexcept = default;
SourceFile::~SourceFile() = default;

std::expected<SourceFile, SourceLoadError> SourceFile::load(
    const std::filesystem::path& path) {
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected(
        SourceLoadError{path, "could not open source file for reading"});
  }

  std::string contents{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
  if (input.bad()) {
    return std::unexpected(
        SourceLoadError{path, "failed while reading source file"});
  }

  return from_memory(path, std::move(contents));
}

SourceFile SourceFile::from_memory(std::filesystem::path path,
                                   std::string contents) {
  auto display_path = path.generic_string();
  if (display_path.empty()) {
    display_path = "<memory>";
  }

  return SourceFile{std::make_unique<Storage>(
      Storage{std::move(path), std::move(display_path), std::move(contents)})};
}

const std::filesystem::path& SourceFile::path() const noexcept {
  return storage_->path;
}

std::string_view SourceFile::display_path() const noexcept {
  return storage_->display_path;
}

std::string_view SourceFile::contents() const noexcept {
  return storage_->contents;
}

}  // namespace cloth
