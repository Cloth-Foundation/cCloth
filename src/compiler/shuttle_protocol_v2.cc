// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/shuttle_protocol_v2.h"

#include "cloth/identity/package_identity.h"
#include "cloth/lexer/token.h"
#include "cloth/source/path.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cloth {
namespace {

struct ParsedArguments {
  std::optional<std::string> protocol;
  std::optional<std::string> operation;
  std::optional<std::string> target;
  std::optional<std::string> artifact_kind;
  std::optional<std::filesystem::path> output;
  std::optional<std::filesystem::path> input;
  std::optional<std::string> root_package;
  std::optional<std::string> entry;
  std::vector<std::tuple<std::string, std::string, std::filesystem::path>>
      packages;
  std::vector<ShuttleV2DependencyInput> dependencies;
  std::vector<ShuttleV2ArtifactInput> artifacts;
};

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
  if (result.empty() || std::ranges::any_of(result, [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return value < 0x20 || value == 0x7f;
      })) {
    return std::nullopt;
  }
  return result;
}

std::expected<std::string, std::string> required_text(
    const std::filesystem::path& argument, std::string_view description) {
  const auto text = argument_text(argument);
  if (!text) {
    return std::unexpected(std::string{description} +
                           " must be nonempty UTF-8 without controls");
  }
  return *text;
}

template <typename T>
std::expected<void, std::string> set_once(std::optional<T>& destination,
                                          T value, std::string_view option) {
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
    if (!option) return std::unexpected(option.error());
    auto values = [&](std::size_t count)
        -> std::expected<std::span<const std::filesystem::path>, std::string> {
      if (count > arguments.size() - index - 1) {
        return std::unexpected(*option + " is missing a value");
      }
      return arguments.subspan(index + 1, count);
    };
    if (*option == "--package") {
      const auto group = values(3);
      if (!group) return std::unexpected(group.error());
      const auto name = required_text((*group)[0], "package name");
      const auto version = required_text((*group)[1], "package version");
      if (!name) return std::unexpected(name.error());
      if (!version) return std::unexpected(version.error());
      parsed.packages.emplace_back(*name, *version, (*group)[2]);
      index += 4;
      continue;
    }
    if (*option == "--dependency") {
      const auto group = values(2);
      if (!group) return std::unexpected(group.error());
      const auto alias = required_text((*group)[0], "dependency alias");
      const auto package = required_text((*group)[1], "dependency package");
      if (!alias) return std::unexpected(alias.error());
      if (!package) return std::unexpected(package.error());
      parsed.dependencies.push_back({*alias, *package});
      index += 3;
      continue;
    }
    if (*option == "--artifact") {
      const auto group = values(4);
      if (!group) return std::unexpected(group.error());
      const auto name = required_text((*group)[0], "artifact package name");
      const auto version =
          required_text((*group)[1], "artifact package version");
      const auto digest = required_text((*group)[2], "artifact digest");
      if (!name) return std::unexpected(name.error());
      if (!version) return std::unexpected(version.error());
      if (!digest) return std::unexpected(digest.error());
      const auto parsed_digest = parse_artifact_digest(*digest);
      if (!parsed_digest) {
        return std::unexpected(
            "artifact digest must be 64 lowercase hex digits");
      }
      parsed.artifacts.push_back(
          {{*name, *version}, *parsed_digest, (*group)[3]});
      index += 5;
      continue;
    }

    const auto group = values(1);
    if (!group) return std::unexpected(group.error());
    std::expected<void, std::string> result;
    if (*option == "--shuttle-protocol" || *option == "--operation" ||
        *option == "--target" || *option == "--artifact-kind" ||
        *option == "--root-package" || *option == "--entry") {
      const auto value = required_text((*group)[0], *option);
      if (!value) return std::unexpected(value.error());
      if (*option == "--shuttle-protocol") {
        result = set_once(parsed.protocol, *value, *option);
      } else if (*option == "--operation") {
        result = set_once(parsed.operation, *value, *option);
      } else if (*option == "--target") {
        result = set_once(parsed.target, *value, *option);
      } else if (*option == "--artifact-kind") {
        result = set_once(parsed.artifact_kind, *value, *option);
      } else if (*option == "--root-package") {
        result = set_once(parsed.root_package, *value, *option);
      } else {
        result = set_once(parsed.entry, *value, *option);
      }
    } else if (*option == "--output") {
      result = set_once(parsed.output, (*group)[0], *option);
    } else if (*option == "--input") {
      result = set_once(parsed.input, (*group)[0], *option);
    } else {
      return std::unexpected("unknown protocol option '" + *option + "'");
    }
    if (!result) return std::unexpected(result.error());
    index += 2;
  }
  return parsed;
}

std::expected<TargetDataLayout, std::string> parse_target(
    const std::optional<std::string>& target) {
  if (!target) return std::unexpected("--target is required");
  if (*target == "x86_64") return TargetDataLayout::llvm_x86_64();
  if (*target == "wasm32") return TargetDataLayout::llvm_wasm32();
  return std::unexpected("unsupported target '" + *target + "'");
}

bool valid_alias(std::string_view value) {
  return !value.empty() && value.front() >= 'a' && value.front() <= 'z' &&
         identifier_token_kind(value) == TokenKind::kIdentifier &&
         std::ranges::all_of(value, [](char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_';
         });
}

std::expected<void, std::string> validate_logical_path(
    std::string_view path, std::string_view description) {
  if (path.empty() || path.starts_with('/') || path.ends_with('/') ||
      path.contains('\\') || path.contains("//")) {
    return std::unexpected(std::string{description} +
                           " must be a normalized relative '/' path");
  }
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t end = path.find('/', begin);
    const std::string_view component =
        path.substr(begin, end == std::string_view::npos ? path.size() - begin
                                                         : end - begin);
    if (component == "." || component == "..") {
      return std::unexpected(std::string{description} +
                             " contains a forbidden path component");
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return {};
}

std::expected<std::filesystem::path, std::string> input_file(
    const std::filesystem::path& path, std::string_view description) {
  if (!path.is_absolute()) {
    return std::unexpected(std::string{description} + " must be absolute");
  }
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_regular_file(canonical, error) || error) {
    return std::unexpected("could not resolve " + std::string{description} +
                           " '" + path_to_utf8(path) + "'");
  }
  return canonical;
}

std::expected<std::filesystem::path, std::string> input_directory(
    const std::filesystem::path& path, std::string_view description) {
  if (!path.is_absolute()) {
    return std::unexpected(std::string{description} + " must be absolute");
  }
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || !std::filesystem::is_directory(canonical, error) || error) {
    return std::unexpected("could not resolve " + std::string{description} +
                           " '" + path_to_utf8(path) + "'");
  }
  return canonical;
}

std::expected<std::filesystem::path, std::string> output_file(
    const std::optional<std::filesystem::path>& path) {
  if (!path || !path->is_absolute()) {
    return std::unexpected("--output must be an absolute path");
  }
  std::error_code error;
  if (!std::filesystem::is_directory(path->parent_path(), error) || error) {
    return std::unexpected("output parent directory does not exist");
  }
  if (std::filesystem::is_directory(*path, error) && !error) {
    return std::unexpected("output path names a directory");
  }
  return path->lexically_normal();
}

std::expected<void, std::string> validate_artifacts(
    std::vector<ShuttleV2ArtifactInput>& artifacts) {
  std::ranges::sort(artifacts, {}, [](const ShuttleV2ArtifactInput& artifact) {
    return artifact.package.name;
  });
  std::set<std::string, std::less<>> names;
  for (ShuttleV2ArtifactInput& artifact : artifacts) {
    if (!is_valid_package_name(artifact.package.name) ||
        !is_valid_package_version(artifact.package.version) ||
        !names.insert(artifact.package.name).second) {
      return std::unexpected(
          "artifact package identities are invalid or duplicate");
    }
    auto path = input_file(artifact.path, "artifact input");
    if (!path) return std::unexpected(path.error());
    artifact.path = std::move(*path);
  }
  return {};
}

struct PackageOperationInputs {
  TargetDataLayout target;
  PackageArtifactKind artifact_kind;
  PackageIdentity package;
  std::filesystem::path source_root;
  std::optional<std::string> entry;
  std::vector<ShuttleV2DependencyInput> dependencies;
  std::vector<ShuttleV2ArtifactInput> artifacts;
};

std::expected<PackageOperationInputs, std::string> package_operation_inputs(
    ParsedArguments parsed) {
  if (parsed.input || parsed.output || parsed.root_package ||
      parsed.packages.size() != 1) {
    return std::unexpected(
        "package operation requires exactly one source package");
  }
  auto target = parse_target(parsed.target);
  if (!target) return std::unexpected(target.error());
  PackageArtifactKind kind;
  if (parsed.artifact_kind == "interface") {
    kind = PackageArtifactKind::kInterface;
  } else if (parsed.artifact_kind == "object") {
    kind = PackageArtifactKind::kObject;
  } else {
    return std::unexpected("--artifact-kind must be 'interface' or 'object'");
  }
  if (kind == PackageArtifactKind::kObject &&
      target->target_name != "x86_64-unknown-unknown") {
    return std::unexpected(
        "object artifacts currently require target 'x86_64'");
  }
  auto [name, version, source_root] = std::move(parsed.packages.front());
  if (has_reserved_standard_library_root(name) &&
      name != kStandardLibraryPackageName) {
    return std::unexpected("standard library package must be spelled 'cloth'");
  }
  if (!is_valid_package_name(name) || !is_valid_package_version(version)) {
    return std::unexpected("source package identity is invalid");
  }
  auto root = input_directory(source_root, "package source root");
  if (!root) return std::unexpected(root.error());
  if (parsed.entry) {
    if (auto valid = validate_logical_path(*parsed.entry, "entry path");
        !valid) {
      return std::unexpected(valid.error());
    }
  }
  std::ranges::sort(parsed.dependencies, {}, &ShuttleV2DependencyInput::alias);
  std::set<std::string, std::less<>> aliases;
  for (const ShuttleV2DependencyInput& dependency : parsed.dependencies) {
    if (has_standard_library_dependency_conflict(dependency.alias,
                                                 dependency.package)) {
      return std::unexpected(
          "standard library dependency must map alias 'cloth' to package "
          "'cloth'");
    }
    if (!valid_alias(dependency.alias) ||
        !is_valid_package_name(dependency.package) ||
        dependency.package == name ||
        !aliases.insert(dependency.alias).second) {
      return std::unexpected("dependency records are invalid or duplicate");
    }
  }
  if (auto valid = validate_artifacts(parsed.artifacts); !valid) {
    return std::unexpected(valid.error());
  }
  return PackageOperationInputs{std::move(*target),
                                kind,
                                {std::move(name), std::move(version)},
                                std::move(*root),
                                std::move(parsed.entry),
                                std::move(parsed.dependencies),
                                std::move(parsed.artifacts)};
}

std::expected<ShuttleV2CompileRequest, std::string> compile_request(
    ParsedArguments parsed) {
  if (parsed.input || !parsed.output) {
    return std::unexpected("compile requires --output and forbids --input");
  }
  auto output = output_file(parsed.output);
  if (!output) return std::unexpected(output.error());
  parsed.output.reset();
  auto package = package_operation_inputs(std::move(parsed));
  if (!package) return std::unexpected(package.error());
  return ShuttleV2CompileRequest{std::move(package->target),
                                 package->artifact_kind,
                                 std::move(*output),
                                 std::move(package->package),
                                 std::move(package->source_root),
                                 std::move(package->entry),
                                 std::move(package->dependencies),
                                 std::move(package->artifacts)};
}

std::expected<ShuttleV2InspectRequest, std::string> inspect_request(
    ParsedArguments parsed) {
  if (!parsed.input || parsed.target || parsed.artifact_kind || parsed.output ||
      parsed.root_package || parsed.entry || !parsed.packages.empty() ||
      !parsed.dependencies.empty() || !parsed.artifacts.empty()) {
    return std::unexpected("inspect requires only one --input option");
  }
  auto input = input_file(*parsed.input, "artifact input");
  if (!input) return std::unexpected(input.error());
  return ShuttleV2InspectRequest{std::move(*input)};
}

std::expected<ShuttleV2ReuseRequest, std::string> reuse_request(
    ParsedArguments parsed) {
  if (!parsed.input || parsed.output) {
    return std::unexpected("reuse requires --input and forbids --output");
  }
  auto input = input_file(*parsed.input, "candidate artifact");
  if (!input) return std::unexpected(input.error());
  parsed.input.reset();
  auto package = package_operation_inputs(std::move(parsed));
  if (!package) return std::unexpected(package.error());
  return ShuttleV2ReuseRequest{std::move(package->target),
                               package->artifact_kind,
                               std::move(*input),
                               std::move(package->package),
                               std::move(package->source_root),
                               std::move(package->entry),
                               std::move(package->dependencies),
                               std::move(package->artifacts)};
}

std::expected<ShuttleV2LinkRequest, std::string> link_request(
    ParsedArguments parsed) {
  if (parsed.input || parsed.artifact_kind || !parsed.packages.empty() ||
      !parsed.dependencies.empty() || !parsed.root_package || !parsed.entry) {
    return std::unexpected(
        "link requires target, output, root package, entry, and artifacts");
  }
  auto target = parse_target(parsed.target);
  auto output = output_file(parsed.output);
  if (!target) return std::unexpected(target.error());
  if (!output) return std::unexpected(output.error());
  if (target->target_name != "x86_64-unknown-unknown") {
    return std::unexpected("native linking currently requires target 'x86_64'");
  }
  if (!is_valid_package_name(*parsed.root_package)) {
    return std::unexpected("root package name is invalid");
  }
  if (auto valid = validate_logical_path(*parsed.entry, "entry path"); !valid) {
    return std::unexpected(valid.error());
  }
  if (auto valid = validate_artifacts(parsed.artifacts); !valid) {
    return std::unexpected(valid.error());
  }
  if (std::ranges::none_of(parsed.artifacts, [&](const auto& artifact) {
        return artifact.package.name == *parsed.root_package;
      })) {
    return std::unexpected("root package has no artifact record");
  }
  return ShuttleV2LinkRequest{
      std::move(*target), std::move(*output), std::move(*parsed.root_package),
      std::move(*parsed.entry), std::move(parsed.artifacts)};
}

std::string target_name(const TargetDataLayout& target) {
  return target.target_name.starts_with("wasm32") ? "wasm32" : "x86_64";
}

}  // namespace

std::expected<ShuttleV2Request, std::string> prepare_shuttle_v2_request(
    std::span<const std::filesystem::path> arguments) {
  auto parsed = parse_arguments(arguments);
  if (!parsed) return std::unexpected(parsed.error());
  if (parsed->protocol != "2" || !parsed->operation) {
    return std::unexpected("unsupported or missing Shuttle protocol operation");
  }
  if (*parsed->operation == "compile") {
    return compile_request(std::move(*parsed));
  }
  if (*parsed->operation == "inspect") {
    return inspect_request(std::move(*parsed));
  }
  if (*parsed->operation == "link") {
    return link_request(std::move(*parsed));
  }
  if (*parsed->operation == "reuse") {
    return reuse_request(std::move(*parsed));
  }
  return std::unexpected("unsupported operation '" + *parsed->operation + "'");
}

std::string shuttle_capabilities_json(const ArtifactDigest& compiler_id) {
  return "{\"schema\":1,\"protocols\":[1,2],\"artifact_formats\":[" +
         std::to_string(kPackageArtifactFormatVersion) +
         "],\"compiler_id\":\"" + artifact_digest_hex(compiler_id) +
         "\",\"standard_library\":{\"package\":\"" +
         std::string{kStandardLibraryPackageName} + "\",\"version\":\"" +
         std::string{kStandardLibraryPackageVersion} +
         "\"},\"operations\":[\"compile\",\"inspect\",\"link\",\"reuse\"],"
         "\"interface_targets\":[\"x86_64\",\"wasm32\"],"
         "\"object_targets\":[\"x86_64\"]}";
}

std::string shuttle_artifact_receipt_json(const PackageArtifact& artifact,
                                          const ArtifactDigest& artifact_id) {
  std::string result =
      "{\"schema\":1,\"artifact_format\":" +
      std::to_string(kPackageArtifactFormatVersion) + ",\"artifact_id\":\"" +
      artifact_digest_hex(artifact_id) + "\",\"kind\":\"" +
      (artifact.kind == PackageArtifactKind::kObject ? "object" : "interface") +
      "\",\"package\":{\"name\":\"" + artifact.imported.package.name +
      "\",\"version\":\"" + artifact.imported.package.version +
      "\"},\"target\":\"" + target_name(artifact.compatibility.target) +
      "\",\"compiler_id\":\"" +
      artifact_digest_hex(artifact.compatibility.compiler_id) +
      "\",\"dependencies\":[";
  for (std::size_t index = 0; index < artifact.dependencies.size(); ++index) {
    if (index != 0) result += ',';
    const ArtifactDependency& dependency = artifact.dependencies[index];
    result += "{\"alias\":\"" + dependency.alias +
              "\",\"package\":{\"name\":\"" + dependency.package.name +
              "\",\"version\":\"" + dependency.package.version +
              "\"},\"artifact_id\":\"" +
              artifact_digest_hex(dependency.digest) + "\"}";
  }
  result += "]}";
  return result;
}

}  // namespace cloth
