#ifndef CLOTH_SOURCE_SOURCE_FILE_H_
#define CLOTH_SOURCE_SOURCE_FILE_H_

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cloth {

struct SourceLoadError {
  std::filesystem::path path;
  std::string message;
};

class SourceFile {
 public:
  static std::expected<SourceFile, SourceLoadError> load(
      const std::filesystem::path& path);

  // Intended for tests and compiler-generated sources.
  static SourceFile from_memory(std::filesystem::path path,
                                std::string contents);

  SourceFile(SourceFile&&) noexcept;
  SourceFile& operator=(SourceFile&&) noexcept;
  ~SourceFile();

  SourceFile(const SourceFile&) = delete;
  SourceFile& operator=(const SourceFile&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept;
  [[nodiscard]] std::string_view display_path() const noexcept;
  [[nodiscard]] std::string_view contents() const noexcept;

 private:
  struct Storage;

  explicit SourceFile(std::unique_ptr<Storage> storage) noexcept;

  // Indirection keeps lexeme and file-name views valid if SourceFile is moved.
  std::unique_ptr<Storage> storage_;
};

}  // namespace cloth

#endif  // CLOTH_SOURCE_SOURCE_FILE_H_
