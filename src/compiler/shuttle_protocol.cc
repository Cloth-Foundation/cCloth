#include "cloth/compiler/shuttle_protocol.h"

#include "cloth/identity/package_identity.h"
#include "cloth/sema/visibility.h"
#include "cloth/source/path.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace cloth {
namespace {

struct ParsedArguments {
  std::optional<std::string> protocol;
  std::optional<std::string> target;
  std::optional<std::string> output_kind;
  std::optional<std::filesystem::path> output;
  std::optional<std::string> root_package;
  std::optional<std::filesystem::path> entry;
  std::vector<ShuttlePackageInput> packages;
  std::vector<ShuttleDependencyInput> dependencies;
};

bool valid_utf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    if (lead <= 0x7f) {
      length = 1;
      code_point = lead;
    } else if (lead >= 0xc2 && lead <= 0xdf) {
      length = 2;
      code_point = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      length = 3;
      code_point = lead & 0x0f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      length = 4;
      code_point = lead & 0x07;
    } else {
      return false;
    }
    if (index + length > value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto byte = static_cast<unsigned char>(value[index + offset]);
      if ((byte & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (byte & 0x3f);
    }
    if ((length == 3 && code_point < 0x800) ||
        (length == 4 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    index += length;
  }
  return true;
}

std::optional<std::string> argument_text(
    const std::filesystem::path& argument) {
  std::u8string encoded;
  try {
    encoded = argument.u8string();
  } catch (const std::system_error&) {
    return std::nullopt;
  }
  std::string result{reinterpret_cast<const char*>(encoded.data()),
                     encoded.size()};
  if (!valid_utf8(result)) {
    return std::nullopt;
  }
  return result;
}

bool contains_control_character(std::string_view value) {
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char raw_character = value[index];
    const auto character = static_cast<unsigned char>(raw_character);
    if (character < 0x20 || character == 0x7f) {
      return true;
    }
    if (character == 0xc2 && index + 1 < value.size()) {
      const auto continuation = static_cast<unsigned char>(value[index + 1]);
      if (continuation >= 0x80 && continuation <= 0x9f) {
        return true;
      }
    }
  }
  return false;
}

std::expected<std::string, std::string> required_text(
    const std::filesystem::path& argument, std::string_view description) {
  const std::optional<std::string> value = argument_text(argument);
  if (!value || contains_control_character(*value)) {
    return std::unexpected(std::string{description} +
                           " must be valid UTF-8 without control characters");
  }
  return *value;
}

std::expected<void, std::string> set_once(
    std::optional<std::string>& destination, std::string value,
    std::string_view option) {
  if (destination) {
    return std::unexpected(std::string{option} +
                           " was specified more than once");
  }
  destination = std::move(value);
  return {};
}

std::expected<void, std::string> set_once(
    std::optional<std::filesystem::path>& destination,
    std::filesystem::path value, std::string_view option) {
  if (destination) {
    return std::unexpected(std::string{option} +
                           " was specified more than once");
  }
  destination = std::move(value);
  return {};
}

std::expected<ParsedArguments, std::string> parse_arguments(
    std::span<const std::filesystem::path> arguments) {
  ParsedArguments parsed;
  for (std::size_t index = 0; index < arguments.size();) {
    const auto option = required_text(arguments[index], "protocol option");
    if (!option) {
      return std::unexpected(option.error());
    }
    auto require_values = [&](std::size_t count)
        -> std::expected<std::span<const std::filesystem::path>, std::string> {
      if (index + count >= arguments.size()) {
        return std::unexpected(*option + " is missing a value");
      }
      return arguments.subspan(index + 1, count);
    };

    if (*option == "--package") {
      const auto values = require_values(3);
      if (!values) {
        return std::unexpected(values.error());
      }
      const auto name = required_text((*values)[0], "package name");
      const auto version = required_text((*values)[1], "package version");
      if (!name) {
        return std::unexpected(name.error());
      }
      if (!version) {
        return std::unexpected(version.error());
      }
      parsed.packages.push_back(
          ShuttlePackageInput{*name, *version, (*values)[2]});
      index += 4;
      continue;
    }
    if (*option == "--dependency") {
      const auto values = require_values(3);
      if (!values) {
        return std::unexpected(values.error());
      }
      const auto owner = required_text((*values)[0], "dependency owner");
      const auto alias = required_text((*values)[1], "dependency alias");
      const auto target = required_text((*values)[2], "dependency target");
      if (!owner) {
        return std::unexpected(owner.error());
      }
      if (!alias) {
        return std::unexpected(alias.error());
      }
      if (!target) {
        return std::unexpected(target.error());
      }
      parsed.dependencies.push_back(
          ShuttleDependencyInput{*owner, *alias, *target});
      index += 4;
      continue;
    }

    if (*option != "--shuttle-protocol" && *option != "--target" &&
        *option != "--output-kind" && *option != "--output" &&
        *option != "--root-package" && *option != "--entry") {
      return std::unexpected("unknown protocol option '" + *option + "'");
    }

    const auto values = require_values(1);
    if (!values) {
      return std::unexpected(values.error());
    }
    std::expected<void, std::string> result;
    if (*option == "--shuttle-protocol") {
      const auto value = required_text((*values)[0], "protocol version");
      if (!value) {
        return std::unexpected(value.error());
      }
      result = set_once(parsed.protocol, *value, *option);
    } else if (*option == "--target") {
      const auto value = required_text((*values)[0], "target");
      if (!value) {
        return std::unexpected(value.error());
      }
      result = set_once(parsed.target, *value, *option);
    } else if (*option == "--output-kind") {
      const auto value = required_text((*values)[0], "output kind");
      if (!value) {
        return std::unexpected(value.error());
      }
      result = set_once(parsed.output_kind, *value, *option);
    } else if (*option == "--output") {
      result = set_once(parsed.output, (*values)[0], *option);
    } else if (*option == "--root-package") {
      const auto value = required_text((*values)[0], "root package");
      if (!value) {
        return std::unexpected(value.error());
      }
      result = set_once(parsed.root_package, *value, *option);
    } else if (*option == "--entry") {
      result = set_once(parsed.entry, (*values)[0], *option);
    }
    if (!result) {
      return std::unexpected(result.error());
    }
    index += 2;
  }
  return parsed;
}

bool valid_dependency_alias(std::string_view value) {
  if (value.empty() || value.front() < 'a' || value.front() > 'z') {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

bool is_keyword(std::string_view value) {
  constexpr std::array<std::string_view, 50> kKeywords{
      "abstract", "as",     "bool",     "break",     "byte",    "char",
      "class",    "const",  "continue", "else",      "enum",    "extern",
      "false",    "final",  "float",    "float32",   "float64", "for",
      "func",     "if",     "import",   "in",        "int",     "int8",
      "int16",    "int32",  "int64",    "interface", "is",      "let",
      "match",    "null",   "object",   "override",  "return",  "sealed",
      "static",   "struct", "super",    "trait",     "true",    "uint",
      "uint8",    "uint16", "uint32",   "uint64",    "unsafe",  "var",
      "void",     "while"};
  return std::ranges::binary_search(kKeywords, value);
}

std::expected<std::string, std::string> path_text(
    const std::filesystem::path& path, std::string_view description) {
  const auto text = required_text(path, description);
  if (!text) {
    return std::unexpected(text.error());
  }
  return *text;
}

bool is_within(const std::filesystem::path& root,
               const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(path, root, error);
  if (error || relative.is_absolute()) {
    return false;
  }
  return std::ranges::none_of(
      relative, [](const auto& component) { return component == ".."; });
}

std::expected<std::filesystem::path, std::string> canonical_directory(
    const std::filesystem::path& path, std::string_view description) {
  const auto text = path_text(path, description);
  if (!text) {
    return std::unexpected(text.error());
  }
  if (!path.is_absolute()) {
    return std::unexpected(std::string{description} + " must be absolute");
  }
  std::error_code error;
  std::filesystem::path result = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_directory(result, error) || error) {
    return std::unexpected("could not resolve " + std::string{description} +
                           " '" + *text + "'");
  }
  if (const auto resolved_text = path_text(result, description);
      !resolved_text) {
    return std::unexpected(resolved_text.error());
  }
  return result;
}

std::expected<void, std::string> validate_logical_path(
    const std::filesystem::path& path, std::string_view description) {
  const auto text = path_text(path, description);
  if (!text) {
    return std::unexpected(text.error());
  }
  if (text->empty() || text->contains('\\') || text->starts_with('/') ||
      text->ends_with('/') || text->contains("//") || path.is_absolute()) {
    return std::unexpected(std::string{description} +
                           " must be a normalized relative '/' path");
  }
  std::size_t begin = 0;
  while (begin <= text->size()) {
    const std::size_t end = text->find('/', begin);
    const std::string_view component = std::string_view{*text}.substr(
        begin, end == std::string::npos ? text->size() - begin : end - begin);
    if (component == "." || component == "..") {
      return std::unexpected(std::string{description} +
                             " must not contain '.' or '..' components");
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return {};
}

std::expected<ShuttleBuildRequest, std::string> validate_request(
    ParsedArguments parsed) {
  if (!parsed.protocol || *parsed.protocol != "1") {
    return std::unexpected("unsupported or missing Shuttle protocol version");
  }
  if (!parsed.target) {
    return std::unexpected("--target is required in protocol mode");
  }
  TargetDataLayout target;
  if (*parsed.target == "x86_64") {
    target = TargetDataLayout::llvm_x86_64();
  } else if (*parsed.target == "wasm32") {
    target = TargetDataLayout::llvm_wasm32();
  } else {
    return std::unexpected("unsupported target '" + *parsed.target + "'");
  }
  if (!parsed.output_kind) {
    return std::unexpected("--output-kind is required in protocol mode");
  }
  ShuttleOutputKind output_kind;
  if (*parsed.output_kind == "check") {
    output_kind = ShuttleOutputKind::kCheck;
  } else if (*parsed.output_kind == "llvm-ir") {
    output_kind = ShuttleOutputKind::kLlvmIr;
  } else if (*parsed.output_kind == "executable") {
    output_kind = ShuttleOutputKind::kExecutable;
  } else {
    return std::unexpected("unsupported output kind '" + *parsed.output_kind +
                           "'");
  }
  if (output_kind == ShuttleOutputKind::kCheck && parsed.output) {
    return std::unexpected("--output is forbidden for check output");
  }
  if (output_kind != ShuttleOutputKind::kCheck && !parsed.output) {
    return std::unexpected("--output is required for file-producing output");
  }
  if (parsed.output && !parsed.output->is_absolute()) {
    return std::unexpected("--output must be an absolute path");
  }
  if (parsed.output) {
    const auto output_text = path_text(*parsed.output, "output path");
    if (!output_text) {
      return std::unexpected(output_text.error());
    }
    std::error_code error;
    if (!std::filesystem::is_directory(parsed.output->parent_path(), error) ||
        error) {
      return std::unexpected("output parent directory does not exist");
    }
  }
  if (output_kind == ShuttleOutputKind::kExecutable && !parsed.entry) {
    return std::unexpected("--entry is required for executable output");
  }
  if (output_kind == ShuttleOutputKind::kExecutable &&
      *parsed.target != "x86_64") {
    return std::unexpected(
        "native executable output currently supports only target 'x86_64'");
  }
  if (!parsed.root_package || !is_valid_package_name(*parsed.root_package)) {
    return std::unexpected("--root-package is missing or invalid");
  }
  if (parsed.packages.empty()) {
    return std::unexpected("at least one --package record is required");
  }

  std::map<std::string, std::filesystem::path> package_roots;
  std::set<std::filesystem::path> unique_roots;
  for (ShuttlePackageInput& package : parsed.packages) {
    if (!is_valid_package_name(package.name)) {
      return std::unexpected("invalid package name '" + package.name + "'");
    }
    if (!is_valid_package_version(package.version)) {
      return std::unexpected("invalid package version '" + package.version +
                             "' for package '" + package.name + "'");
    }
    auto root = canonical_directory(package.source_root, "package source root");
    if (!root) {
      return std::unexpected(root.error());
    }
    package.source_root = std::move(*root);
    if (!package_roots.emplace(package.name, package.source_root).second) {
      return std::unexpected("duplicate package record '" + package.name + "'");
    }
    if (!unique_roots.insert(package.source_root).second) {
      return std::unexpected("two package records use the same source root");
    }
  }
  if (!package_roots.contains(*parsed.root_package)) {
    return std::unexpected("root package '" + *parsed.root_package +
                           "' has no package record");
  }

  std::set<std::pair<std::string, std::string>> aliases;
  std::set<std::pair<std::string, std::string>> targets;
  for (const ShuttleDependencyInput& dependency : parsed.dependencies) {
    if (!package_roots.contains(dependency.owner) ||
        !package_roots.contains(dependency.target)) {
      return std::unexpected("dependency '" + dependency.owner + " --" +
                             dependency.alias + "--> " + dependency.target +
                             "' refers to an unknown package");
    }
    if (!valid_dependency_alias(dependency.alias) ||
        is_keyword(dependency.alias)) {
      return std::unexpected("invalid dependency alias '" + dependency.alias +
                             "'");
    }
    if (dependency.owner == dependency.target) {
      return std::unexpected("package '" + dependency.owner +
                             "' cannot depend on itself");
    }
    if (!aliases.emplace(dependency.owner, dependency.alias).second) {
      return std::unexpected("duplicate dependency alias '" + dependency.alias +
                             "' for package '" + dependency.owner + "'");
    }
    if (!targets.emplace(dependency.owner, dependency.target).second) {
      return std::unexpected("package '" + dependency.owner +
                             "' exposes one dependency through two aliases");
    }
  }

  std::ranges::sort(parsed.packages, {}, &ShuttlePackageInput::name);
  std::ranges::sort(parsed.dependencies,
                    [](const auto& left, const auto& right) {
                      return std::tie(left.owner, left.alias) <
                             std::tie(right.owner, right.alias);
                    });
  return ShuttleBuildRequest{std::move(target),
                             output_kind,
                             std::move(parsed.output),
                             std::move(*parsed.root_package),
                             std::move(parsed.entry),
                             std::move(parsed.packages),
                             std::move(parsed.dependencies)};
}

std::string source_package_name(const std::filesystem::path& relative) {
  std::string result;
  for (const std::filesystem::path& component : relative.parent_path()) {
    if (!result.empty()) {
      result += '.';
    }
    result += path_to_utf8(component);
  }
  return result;
}

std::string logical_source_name(std::string_view package,
                                std::string_view source_package,
                                std::string_view stem) {
  std::string result{package};
  result += '.';
  if (!source_package.empty()) {
    result += source_package;
    result += '.';
  }
  result += stem;
  return result;
}

std::string ascii_case_key(std::string value) {
  for (char& character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 'A' && byte <= 'Z') {
      character = static_cast<char>(byte + ('a' - 'A'));
    }
  }
  return value;
}

std::expected<void, std::string> validate_source_directory(
    const std::filesystem::path& relative, std::string_view package) {
  if (!is_valid_identifier(path_to_utf8(relative.filename()))) {
    return std::unexpected("package '" + std::string{package} +
                           "' has invalid source directory '" +
                           path_to_utf8(relative) + "'");
  }
  return {};
}

std::expected<std::vector<ShuttleSourceInput>, std::string> enumerate_sources(
    const ShuttleBuildRequest& request) {
  std::vector<ShuttleSourceInput> sources;
  std::map<std::string, std::string> logical_sources;
  std::map<std::filesystem::path, std::string> physical_sources;
  std::map<std::string, std::set<std::string>> top_level_packages;

  for (const ShuttlePackageInput& package : request.packages) {
    std::set<std::filesystem::path> pending_directories{{}};
    std::size_t package_source_count = 0;
    while (!pending_directories.empty()) {
      const std::filesystem::path relative_directory =
          *pending_directories.begin();
      pending_directories.erase(pending_directories.begin());
      std::error_code error;
      std::filesystem::directory_iterator iterator{
          package.source_root / relative_directory, error};
      const std::filesystem::directory_iterator end;
      std::vector<std::filesystem::directory_entry> entries;
      while (!error && iterator != end) {
        if (const auto text = path_text(iterator->path(), "source path");
            !text) {
          return std::unexpected(text.error());
        }
        entries.push_back(*iterator);
        iterator.increment(error);
      }
      if (error) {
        return std::unexpected("could not enumerate package '" + package.name +
                               "': " + error.message());
      }
      std::ranges::sort(entries, {}, [](const auto& entry) {
        return path_to_utf8(entry.path().filename());
      });

      for (const std::filesystem::directory_entry& entry : entries) {
        const std::filesystem::path relative =
            relative_directory / entry.path().filename();
        const bool is_directory = entry.is_directory(error);
        if (error) {
          return std::unexpected("could not inspect source '" +
                                 path_to_utf8(entry.path()) + "'");
        }
        if (is_directory) {
          const auto valid = validate_source_directory(relative, package.name);
          if (!valid) {
            return std::unexpected(valid.error());
          }
          top_level_packages[package.name].insert(
              path_to_utf8(*relative.begin()));
          if (!entry.is_symlink(error) && !error) {
            pending_directories.insert(relative);
          }
          if (error) {
            return std::unexpected("could not inspect source directory '" +
                                   path_to_utf8(entry.path()) + "'");
          }
          continue;
        }
        if (!entry.is_regular_file(error) || error ||
            entry.path().extension() != ".co") {
          if (error) {
            return std::unexpected("could not inspect source file '" +
                                   path_to_utf8(entry.path()) + "'");
          }
          continue;
        }

        const std::string stem = path_to_utf8(entry.path().stem());
        if (!is_valid_identifier(stem)) {
          return std::unexpected("package '" + package.name +
                                 "' has invalid Cloth file stem '" + stem +
                                 "'");
        }
        std::filesystem::path canonical =
            std::filesystem::canonical(entry.path(), error);
        if (error || !is_within(package.source_root, canonical)) {
          return std::unexpected("source '" + path_to_utf8(entry.path()) +
                                 "' escapes package source root");
        }
        if (const auto text = path_text(canonical, "source path"); !text) {
          return std::unexpected(text.error());
        }
        const std::string source_package = source_package_name(relative);
        const std::string logical =
            logical_source_name(package.name, source_package, stem);
        const std::string logical_key = ascii_case_key(logical);
        if (const auto previous = logical_sources.find(logical_key);
            previous != logical_sources.end()) {
          return std::unexpected("source '" + logical +
                                 "' collides by ASCII case with '" +
                                 previous->second + "'");
        }
        logical_sources.emplace(logical_key, logical);
        if (const auto previous = physical_sources.find(canonical);
            previous != physical_sources.end()) {
          return std::unexpected("source file '" + path_to_utf8(canonical) +
                                 "' is owned by both '" + previous->second +
                                 "' and '" + package.name + "'");
        }
        physical_sources.emplace(canonical, package.name);
        auto source = SourceFile::load(canonical);
        if (!source) {
          return std::unexpected(path_to_utf8(source.error().path) + ": " +
                                 source.error().message);
        }
        sources.push_back(ShuttleSourceInput{std::move(*source), package.name,
                                             std::move(source_package),
                                             package.version});
        ++package_source_count;
      }
    }
    if (package_source_count == 0) {
      return std::unexpected("package '" + package.name +
                             "' source root contains no Cloth files");
    }
  }

  for (const ShuttleDependencyInput& dependency : request.dependencies) {
    if (top_level_packages[dependency.owner].contains(dependency.alias)) {
      return std::unexpected("dependency alias '" + dependency.alias +
                             "' collides with a local source package in '" +
                             dependency.owner + "'");
    }
  }
  std::ranges::sort(sources, [](const auto& left, const auto& right) {
    return logical_source_name(left.package, left.source_package,
                               left.source.stem()) <
           logical_source_name(right.package, right.source_package,
                               right.source.stem());
  });
  return sources;
}

std::expected<std::optional<std::string>, std::string> validate_entry(
    const ShuttleBuildRequest& request,
    std::span<const ShuttleSourceInput> sources) {
  if (!request.entry) {
    return std::optional<std::string>{};
  }
  const auto valid = validate_logical_path(*request.entry, "entry path");
  if (!valid) {
    return std::unexpected(valid.error());
  }
  if (request.entry->extension() != ".co") {
    return std::unexpected("entry path must use the exact '.co' extension");
  }
  const auto root = std::ranges::find(request.packages, request.root_package,
                                      &ShuttlePackageInput::name);
  if (root == request.packages.end()) {
    return std::unexpected("root package has no package record");
  }
  std::error_code error;
  const std::filesystem::path entry =
      std::filesystem::canonical(root->source_root / *request.entry, error);
  if (error || !std::filesystem::is_regular_file(entry, error) || error ||
      !is_within(root->source_root, entry)) {
    return std::unexpected(
        "entry path does not resolve to a source file in "
        "the root package");
  }
  if (std::ranges::none_of(sources, [&](const ShuttleSourceInput& source) {
        return source.package == request.root_package &&
               source.source.path() == entry;
      })) {
    return std::unexpected("entry path is not an eligible Cloth source file");
  }
  return logical_source_name(request.root_package,
                             source_package_name(*request.entry),
                             path_to_utf8(request.entry->stem()));
}

std::expected<void, std::string> validate_acyclic(
    const ShuttleBuildRequest& request) {
  std::map<std::string, std::size_t> indegrees;
  std::map<std::string, std::vector<std::string>> dependents;
  for (const ShuttlePackageInput& package : request.packages) {
    indegrees.emplace(package.name, 0);
  }
  for (const ShuttleDependencyInput& dependency : request.dependencies) {
    ++indegrees[dependency.owner];
    dependents[dependency.target].push_back(dependency.owner);
  }
  std::set<std::string> ready;
  for (const auto& [package, indegree] : indegrees) {
    if (indegree == 0) {
      ready.insert(package);
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const std::string package = *ready.begin();
    ready.erase(ready.begin());
    ++visited;
    for (const std::string& dependent : dependents[package]) {
      if (--indegrees[dependent] == 0) {
        ready.insert(dependent);
      }
    }
  }
  if (visited != request.packages.size()) {
    return std::unexpected("dependency graph contains a cycle");
  }
  return {};
}

}  // namespace

std::expected<ShuttleBuildPlan, std::string> prepare_shuttle_build(
    std::span<const std::filesystem::path> arguments) {
  auto parsed = parse_arguments(arguments);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }
  auto request = validate_request(std::move(*parsed));
  if (!request) {
    return std::unexpected(request.error());
  }
  const auto acyclic = validate_acyclic(*request);
  if (!acyclic) {
    return std::unexpected(acyclic.error());
  }
  auto sources = enumerate_sources(*request);
  if (!sources) {
    return std::unexpected(sources.error());
  }
  auto entry = validate_entry(*request, *sources);
  if (!entry) {
    return std::unexpected(entry.error());
  }
  return ShuttleBuildPlan{std::move(*request), std::move(*sources),
                          std::move(*entry)};
}

}  // namespace cloth
