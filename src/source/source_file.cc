// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/source/source_file.h"

#include "cloth/source/path.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace cloth {

struct SourceFile::Storage {
  std::filesystem::path path;
  std::string display_path;
  std::string stem;
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

  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  constexpr std::uintmax_t kMaximumRead =
      static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max());
  if (error || size > kMaximumRead ||
      size > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(
        SourceLoadError{path, "source file size could not be read"});
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    return std::unexpected(
        SourceLoadError{path, "source file changed or failed while reading"});
  }

  return from_memory(path, std::move(contents));
}

SourceFile SourceFile::from_memory(std::filesystem::path path,
                                   std::string contents) {
  auto display_path = path_to_utf8(path);
  if (display_path.empty()) {
    display_path = "<memory>";
  }

  auto stem = path_to_utf8(path.stem());
  return SourceFile{std::make_unique<Storage>(
      Storage{std::move(path), std::move(display_path), std::move(stem),
              std::move(contents)})};
}

const std::filesystem::path& SourceFile::path() const noexcept {
  return storage_->path;
}

std::string_view SourceFile::display_path() const noexcept {
  return storage_->display_path;
}

std::string_view SourceFile::stem() const noexcept { return storage_->stem; }

std::string_view SourceFile::contents() const noexcept {
  return storage_->contents;
}

}  // namespace cloth
