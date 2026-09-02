// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_COMPILER_SHUTTLE_PROTOCOL_V2_H_
#define CLOTH_COMPILER_SHUTTLE_PROTOCOL_V2_H_

#include "cloth/artifact/package_artifact.h"

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace cloth {

inline constexpr std::uint32_t kShuttleProtocolV2Schema = 1;

struct ShuttleV2ArtifactInput {
  PackageIdentity package;
  ArtifactDigest digest;
  std::filesystem::path path;
};

struct ShuttleV2DependencyInput {
  std::string alias;
  std::string package;
};

struct ShuttleV2CompileRequest {
  TargetDataLayout target;
  PackageArtifactKind artifact_kind;
  std::filesystem::path output;
  PackageIdentity package;
  std::filesystem::path source_root;
  std::optional<std::string> entry;
  std::vector<ShuttleV2DependencyInput> dependencies;
  std::vector<ShuttleV2ArtifactInput> artifacts;
};

struct ShuttleV2InspectRequest {
  std::filesystem::path input;
};

struct ShuttleV2ReuseRequest {
  TargetDataLayout target;
  PackageArtifactKind artifact_kind;
  std::filesystem::path input;
  PackageIdentity package;
  std::filesystem::path source_root;
  std::optional<std::string> entry;
  std::vector<ShuttleV2DependencyInput> dependencies;
  std::vector<ShuttleV2ArtifactInput> artifacts;
};

struct ShuttleV2LinkRequest {
  TargetDataLayout target;
  std::filesystem::path output;
  std::string root_package;
  std::string entry;
  std::vector<ShuttleV2ArtifactInput> artifacts;
};

using ShuttleV2Request =
    std::variant<ShuttleV2CompileRequest, ShuttleV2InspectRequest,
                 ShuttleV2ReuseRequest, ShuttleV2LinkRequest>;

[[nodiscard]] std::expected<ShuttleV2Request, std::string>
prepare_shuttle_v2_request(std::span<const std::filesystem::path> arguments);

[[nodiscard]] std::string shuttle_capabilities_json(
    const ArtifactDigest& compiler_id);

[[nodiscard]] std::string shuttle_artifact_receipt_json(
    const PackageArtifact& artifact, const ArtifactDigest& artifact_id);

}  // namespace cloth

#endif  // CLOTH_COMPILER_SHUTTLE_PROTOCOL_V2_H_
