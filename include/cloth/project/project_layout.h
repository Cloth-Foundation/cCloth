#ifndef CLOTH_PROJECT_PROJECT_LAYOUT_H_
#define CLOTH_PROJECT_PROJECT_LAYOUT_H_

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace cloth {

struct ProjectLayout {
  std::filesystem::path project_root;
  std::filesystem::path source_root;
  std::optional<std::filesystem::path> manifest_path;
};

[[nodiscard]] std::expected<ProjectLayout, std::string> discover_project_layout(
    const std::filesystem::path& entry_source);

}  // namespace cloth

#endif  // CLOTH_PROJECT_PROJECT_LAYOUT_H_
