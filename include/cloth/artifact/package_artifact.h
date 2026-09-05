// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef CLOTH_ARTIFACT_PACKAGE_ARTIFACT_H_
#define CLOTH_ARTIFACT_PACKAGE_ARTIFACT_H_

#include "cloth/artifact/imported_package.h"
#include "cloth/identity/canonical_identity.h"
#include "cloth/target/data_layout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cloth {

inline constexpr std::uint32_t kPackageArtifactFormatVersion = 5;
inline constexpr std::uint32_t kRuntimeAbiVersion = 4;
inline constexpr std::uint64_t kMaximumArtifactMetadataSize =
    64ULL * 1024 * 1024;
inline constexpr std::uint64_t kMaximumArtifactPayloadSize =
    1024ULL * 1024 * 1024;
inline constexpr std::size_t kMaximumArtifactNesting = 128;

struct ArtifactDigest {
  std::array<std::uint8_t, 32> bytes{};

  friend bool operator==(const ArtifactDigest&,
                         const ArtifactDigest&) = default;
};

[[nodiscard]] ArtifactDigest sha256(std::span<const std::uint8_t> bytes);
[[nodiscard]] ArtifactDigest sha256(std::string_view bytes);
[[nodiscard]] std::string artifact_digest_hex(const ArtifactDigest& digest);
[[nodiscard]] std::optional<ArtifactDigest> parse_artifact_digest(
    std::string_view text);

enum class PackageArtifactKind {
  kInterface,
  kObject,
};

struct ArtifactSource {
  std::string path;
  ArtifactDigest digest;

  friend bool operator==(const ArtifactSource&,
                         const ArtifactSource&) = default;
};

struct ArtifactDependency {
  std::string alias;
  PackageIdentity package;
  ArtifactDigest digest;

  friend bool operator==(const ArtifactDependency&,
                         const ArtifactDependency&) = default;
};

struct ArtifactToolIdentity {
  std::string name;
  ArtifactDigest digest;

  friend bool operator==(const ArtifactToolIdentity&,
                         const ArtifactToolIdentity&) = default;
};

struct ArtifactNativeCompatibility {
  std::string target_triple;
  std::string object_format;
  std::string cpu;
  std::vector<std::string> features;
  std::string relocation_model;
  std::string code_model;
  ArtifactDigest runtime_digest;
  std::vector<ArtifactToolIdentity> tools;

  friend bool operator==(const ArtifactNativeCompatibility&,
                         const ArtifactNativeCompatibility&) = default;
};

struct ArtifactCompatibility {
  std::uint32_t compiler_abi;
  std::uint32_t runtime_abi;
  ArtifactDigest compiler_id;
  TargetDataLayout target;
  std::optional<ArtifactNativeCompatibility> native;

  friend bool operator==(const ArtifactCompatibility&,
                         const ArtifactCompatibility&) = default;
};

enum class ArtifactSymbolRole {
  kDefinition,
  kRequirement,
};

enum class ArtifactSymbolKind {
  kCallable,
  kConstructorInitializer,
  kStaticField,
  kDescriptor,
  kRuntime,
};

struct ArtifactSymbol {
  std::string link_name;
  std::optional<std::string> canonical_identity;
  ArtifactSymbolRole role;
  ArtifactSymbolKind kind;
  std::string abi_signature;

  friend bool operator==(const ArtifactSymbol&,
                         const ArtifactSymbol&) = default;
};

struct PackageArtifact {
  PackageArtifactKind kind;
  ArtifactCompatibility compatibility;
  std::vector<ArtifactSource> sources;
  std::vector<ArtifactDependency> dependencies;
  ImportedPackageView imported;
  std::vector<ArtifactSymbol> symbols;
  std::vector<std::uint8_t> native_payload;

  friend bool operator==(const PackageArtifact&,
                         const PackageArtifact&) = default;
};

enum class ArtifactIssueCode {
  kInvalidModel,
  kLimitExceeded,
  kMalformedEnvelope,
  kIntegrityMismatch,
  kMalformedMetadata,
  kNoncanonicalMetadata,
  kIncompatible,
};

struct ArtifactIssue {
  ArtifactIssueCode code;
  std::string record;
  std::string message;

  friend bool operator==(const ArtifactIssue&, const ArtifactIssue&) = default;
};

struct EncodedPackageArtifact {
  std::vector<std::uint8_t> bytes;
  ArtifactDigest digest;
};

struct PackageArtifactWriteResult {
  std::optional<EncodedPackageArtifact> artifact;
  std::vector<ArtifactIssue> issues;

  [[nodiscard]] bool is_valid() const noexcept {
    return artifact.has_value() && issues.empty();
  }
};

struct PackageArtifactReadResult {
  std::optional<PackageArtifact> artifact;
  std::optional<ArtifactDigest> digest;
  std::vector<ArtifactIssue> issues;

  [[nodiscard]] bool is_valid() const noexcept {
    return artifact.has_value() && digest.has_value() && issues.empty();
  }
};

[[nodiscard]] std::vector<ArtifactIssue> verify_package_artifact(
    const PackageArtifact& artifact);

[[nodiscard]] PackageArtifactWriteResult write_package_artifact(
    const PackageArtifact& artifact);

// expected_compatibility is an exact compatibility gate. Omit it only when
// inspecting an artifact without selecting it as a build input.
[[nodiscard]] PackageArtifactReadResult read_package_artifact(
    std::span<const std::uint8_t> bytes,
    const std::optional<ArtifactCompatibility>& expected_compatibility =
        std::nullopt);

}  // namespace cloth

#endif  // CLOTH_ARTIFACT_PACKAGE_ARTIFACT_H_
