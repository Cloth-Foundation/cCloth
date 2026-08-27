#include "cloth/project/project_layout.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace cloth {
namespace {

bool is_within(const std::filesystem::path& root,
               const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(path, root, error);
  if (error || relative.is_absolute()) {
    return false;
  }
  for (const std::filesystem::path& component : relative) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

}  // namespace

std::expected<ProjectLayout, std::string> discover_project_layout(
    const std::filesystem::path& entry_source) {
  std::error_code error;
  std::filesystem::path entry =
      std::filesystem::absolute(entry_source, error).lexically_normal();
  if (error) {
    return std::unexpected("could not make the entry source path absolute");
  }

  std::filesystem::path directory = entry.parent_path();
  while (!directory.empty()) {
    const std::filesystem::path manifest = directory / "cloth.toml";
    if (std::filesystem::is_regular_file(manifest, error) && !error) {
      const std::filesystem::path source_root = directory / "src";
      if (!std::filesystem::is_directory(source_root, error) || error) {
        return std::unexpected("project manifest requires a 'src' directory");
      }
      if (!is_within(source_root, entry)) {
        return std::unexpected(
            "entry source is outside the project's 'src' directory");
      }
      return ProjectLayout{directory, source_root, manifest};
    }
    error.clear();
    const std::filesystem::path parent = directory.parent_path();
    if (parent == directory) {
      break;
    }
    directory = parent;
  }

  return ProjectLayout{entry.parent_path(), entry.parent_path(), std::nullopt};
}

}  // namespace cloth
