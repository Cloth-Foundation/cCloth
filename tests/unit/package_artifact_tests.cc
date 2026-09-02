// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/package_artifact.h"
#include "cloth/compiler/compilation.h"
#include "cloth/identity/canonical_identity.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

constexpr std::string_view kSource =
    "static final float32 Ratio = 1.5;\n"
    "static final int64 Build = 12;\n"
    "static func Main() {}\n";

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes,
                       std::size_t offset) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    result |= static_cast<std::uint64_t>(bytes[offset + index])
              << static_cast<unsigned int>(index * 8);
  }
  return result;
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset,
               std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::uint8_t>(
        value >> static_cast<unsigned int>(index * 8));
  }
}

std::string metadata_text(const std::vector<std::uint8_t>& bytes) {
  const auto size = static_cast<std::size_t>(read_u64(bytes, 16));
  return std::string{reinterpret_cast<const char*>(bytes.data() + 64), size};
}

void resign(std::vector<std::uint8_t>& bytes) {
  std::fill(bytes.begin() + 32, bytes.begin() + 64, 0);
  const cloth::ArtifactDigest digest = cloth::sha256(bytes);
  std::ranges::copy(digest.bytes, bytes.begin() + 32);
}

std::vector<std::uint8_t> replace_metadata(
    const std::vector<std::uint8_t>& original, std::string metadata) {
  const std::size_t old_size = static_cast<std::size_t>(read_u64(original, 16));
  const std::size_t payload_size =
      static_cast<std::size_t>(read_u64(original, 24));
  std::vector<std::uint8_t> result(64 + metadata.size() + payload_size, 0);
  std::ranges::copy(std::span<const std::uint8_t>{original}.first(64),
                    result.begin());
  write_u64(result, 16, metadata.size());
  std::transform(
      metadata.begin(), metadata.end(), result.begin() + 64,
      [](char character) { return static_cast<std::uint8_t>(character); });
  std::ranges::copy(
      std::span<const std::uint8_t>{original}.subspan(64 + old_size),
      result.begin() + static_cast<std::ptrdiff_t>(64 + metadata.size()));
  resign(result);
  return result;
}

cloth::PackageArtifact make_artifact(
    cloth::PackageArtifactKind kind = cloth::PackageArtifactKind::kInterface) {
  cloth::Compilation compilation;
  compilation.add_package_source(
      cloth::SourceFile::from_memory("relocated/source/Main.co",
                                     std::string{kSource}),
      "sample", "", "1.2.3+fixture");
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  auto imported = cloth::build_imported_package_view(
      {"sample", "1.2.3+fixture"}, result.semantics, result.mir, result.abi);

  cloth::ArtifactCompatibility compatibility{
      cloth::kCompilerAbiVersion, cloth::kRuntimeAbiVersion,
      cloth::sha256("clothc-fixture"), result.abi.target, std::nullopt};
  std::vector<std::uint8_t> payload;
  if (kind == cloth::PackageArtifactKind::kObject) {
    compatibility.native = cloth::ArtifactNativeCompatibility{
        "x86_64-pc-windows-msvc",
        "coff",
        "x86-64",
        {"+sse2"},
        "pic",
        "small",
        cloth::sha256("runtime-fixture"),
        {{"llc", cloth::sha256("llc-fixture")}}};
    payload.assign(20, 0);
    payload[0] = 0x64;
    payload[1] = 0x86;
  }

  std::vector<cloth::ArtifactSymbol> symbols;
  const cloth::ImportedFile& file = imported.view->files[0];
  symbols.push_back(cloth::ArtifactSymbol{
      file.abi.descriptor->mangled_name, file.abi.descriptor->identity,
      cloth::ArtifactSymbolRole::kDefinition,
      cloth::ArtifactSymbolKind::kDescriptor, "descriptor:file_class"});
  for (const cloth::ImportedStaticFieldAbi& field : file.abi.static_fields) {
    symbols.push_back(cloth::ArtifactSymbol{
        field.mangled_name, field.member_identity,
        cloth::ArtifactSymbolRole::kDefinition,
        cloth::ArtifactSymbolKind::kStaticField,
        "global:" + cloth::mangle_canonical_identity(field.type_identity)});
  }
  for (const cloth::ImportedCallableAbi& callable : file.abi.callables) {
    symbols.push_back(cloth::ArtifactSymbol{
        callable.mangled_name, callable.member_identity,
        cloth::ArtifactSymbolRole::kDefinition,
        cloth::ArtifactSymbolKind::kCallable,
        cloth::imported_callable_signature(
            callable.return_mode, callable.receiver_mode,
            callable.return_type_identity, callable.parameters)});
  }
  if (kind == cloth::PackageArtifactKind::kObject) {
    symbols.push_back(cloth::ArtifactSymbol{
        "cloth_gc_allocate", std::nullopt,
        cloth::ArtifactSymbolRole::kRequirement,
        cloth::ArtifactSymbolKind::kRuntime, "c:ptr(i64,ptr)"});
  }
  std::ranges::sort(symbols, {}, &cloth::ArtifactSymbol::link_name);

  return cloth::PackageArtifact{
      kind,
      std::move(compatibility),
      {{"Main.co", cloth::sha256(kSource)}},
      {{"core", {"core", "2.0.0"}, cloth::sha256("core-artifact")}},
      std::move(*imported.view),
      std::move(symbols),
      std::move(payload)};
}

void sha256_vectors(TestContext& test) {
  test.expect(cloth::artifact_digest_hex(cloth::sha256(std::string_view{})) ==
                  "e3b0c44298fc1c149afbf4c8996fb924"
                  "27ae41e4649b934ca495991b7852b855",
              "empty SHA-256 vector does not match FIPS 180-4");
  test.expect(cloth::artifact_digest_hex(cloth::sha256("abc")) ==
                  "ba7816bf8f01cfea414140de5dae2223"
                  "b00361a396177a9cb410ff61f20015ad",
              "abc SHA-256 vector does not match FIPS 180-4");
  test.expect(!cloth::parse_artifact_digest("BA7816BF8F01CFEA414140DE5DAE2223"
                                            "B00361A396177A9CB410FF61F20015AD"),
              "uppercase digest spelling was accepted");
}

void canonical_interface_round_trip(TestContext& test) {
  const cloth::PackageArtifact artifact = make_artifact();
  const auto encoded = cloth::write_package_artifact(artifact);
  test.expect(encoded.is_valid(), "valid interface artifact was not encoded");
  if (!encoded.artifact) return;
  const auto& bytes = encoded.artifact->bytes;
  test.expect(bytes.size() > 64 && bytes[0] == 0x43 && bytes[7] == 0 &&
                  read_u64(bytes, 24) == 0,
              "version-1 envelope fields are incorrect");
  const auto decoded =
      cloth::read_package_artifact(bytes, artifact.compatibility);
  test.expect(decoded.is_valid(), "canonical interface artifact did not read");
  if (!decoded.artifact) return;
  test.expect(
      decoded.digest == encoded.artifact->digest &&
          decoded.artifact->imported.package == artifact.imported.package &&
          decoded.artifact->dependencies == artifact.dependencies &&
          decoded.artifact->symbols == artifact.symbols &&
          decoded.artifact->native_payload.empty(),
      "artifact round trip changed owned metadata");
  const auto reencoded = cloth::write_package_artifact(*decoded.artifact);
  test.expect(reencoded.is_valid() && reencoded.artifact->bytes == bytes,
              "read/write round trip is not byte deterministic");

  const std::string metadata = metadata_text(bytes);
  test.expect(!metadata.empty() && metadata.front() == '{' &&
                  metadata.back() == '}' && !metadata.ends_with('\n') &&
                  metadata.starts_with("{\"compatibility\":") &&
                  metadata.contains("\"value\":\"3fc00000\"") &&
                  !metadata.contains("FileId") && !metadata.contains("Mir"),
              "metadata is not the approved canonical record form");
  test.expect(
      metadata.size() == 12288 &&
          cloth::artifact_digest_hex(cloth::sha256(metadata)) ==
              "22560baef8517f607064e20963034209d22"
              "abc9277e1728b27c985fa3846c504" &&
          cloth::artifact_digest_hex(encoded.artifact->digest) ==
              "8fb4fab3cce5aad2e18efeb038572418d0"
              "f5c9481ea12257c9f518b6efaac3ec",
      "canonical version-3 fixture: size=" + std::to_string(metadata.size()) +
          " metadata=" + cloth::artifact_digest_hex(cloth::sha256(metadata)) +
          " artifact=" + cloth::artifact_digest_hex(encoded.artifact->digest));
}

void object_round_trip_and_compatibility(TestContext& test) {
  const cloth::PackageArtifact artifact =
      make_artifact(cloth::PackageArtifactKind::kObject);
  const auto encoded = cloth::write_package_artifact(artifact);
  test.expect(encoded.is_valid(), "valid object artifact was not encoded");
  if (!encoded.artifact) return;
  test.expect(read_u64(encoded.artifact->bytes, 24) == 20,
              "object payload length is absent from the envelope");
  const auto decoded = cloth::read_package_artifact(encoded.artifact->bytes,
                                                    artifact.compatibility);
  test.expect(decoded.is_valid() &&
                  decoded.artifact->native_payload == artifact.native_payload &&
                  decoded.artifact->compatibility.native ==
                      artifact.compatibility.native,
              "object payload or native compatibility changed on read");
  const auto expect_incompatible = [&](cloth::ArtifactCompatibility expected,
                                       std::string_view description) {
    const auto rejected =
        cloth::read_package_artifact(encoded.artifact->bytes, expected);
    test.expect(
        !rejected.is_valid() && !rejected.issues.empty() &&
            rejected.issues[0].code == cloth::ArtifactIssueCode::kIncompatible,
        std::string{description} + " compatibility mismatch was accepted");
  };
  auto incompatible = artifact.compatibility;
  incompatible.native->cpu = "different";
  expect_incompatible(incompatible, "native configuration");
  incompatible = artifact.compatibility;
  incompatible.native->runtime_digest = cloth::sha256("changed runtime");
  expect_incompatible(incompatible, "runtime");
  incompatible = artifact.compatibility;
  incompatible.native->tools[0].digest = cloth::sha256("changed tool");
  expect_incompatible(incompatible, "native tool");
  auto malformed_object = encoded.artifact->bytes;
  const std::size_t payload_offset =
      64 + static_cast<std::size_t>(read_u64(malformed_object, 16));
  malformed_object[payload_offset] = 0;
  resign(malformed_object);
  test.expect(!cloth::read_package_artifact(malformed_object).is_valid(),
              "object payload with the wrong machine was accepted");
}

void envelope_and_integrity_failures(TestContext& test) {
  const auto encoded = cloth::write_package_artifact(make_artifact());
  if (!encoded.artifact) {
    test.expect(false, "failure fixture could not be encoded");
    return;
  }
  const auto expect_rejected = [&](std::vector<std::uint8_t> bytes,
                                   cloth::ArtifactIssueCode code,
                                   std::string_view message) {
    const auto result = cloth::read_package_artifact(bytes);
    test.expect(!result.is_valid() && !result.issues.empty() &&
                    result.issues[0].code == code,
                message);
  };
  auto broken = encoded.artifact->bytes;
  broken[0] = 0;
  expect_rejected(std::move(broken),
                  cloth::ArtifactIssueCode::kMalformedEnvelope,
                  "bad magic was accepted");
  broken = encoded.artifact->bytes;
  broken[8] = 1;
  expect_rejected(std::move(broken), cloth::ArtifactIssueCode::kIncompatible,
                  "unsupported format version was accepted");
  broken = encoded.artifact->bytes;
  broken[8] = 2;
  expect_rejected(std::move(broken), cloth::ArtifactIssueCode::kIncompatible,
                  "pre-aggregate format version was accepted");
  broken = encoded.artifact->bytes;
  broken[12] = 1;
  expect_rejected(std::move(broken),
                  cloth::ArtifactIssueCode::kMalformedEnvelope,
                  "nonzero reserved flags were accepted");
  broken = encoded.artifact->bytes;
  broken.pop_back();
  expect_rejected(std::move(broken),
                  cloth::ArtifactIssueCode::kMalformedEnvelope,
                  "truncated artifact was accepted");
  broken = encoded.artifact->bytes;
  broken.push_back(0);
  expect_rejected(std::move(broken),
                  cloth::ArtifactIssueCode::kMalformedEnvelope,
                  "artifact trailing byte was accepted");
  broken = encoded.artifact->bytes;
  broken.back() ^= 1U;
  expect_rejected(std::move(broken),
                  cloth::ArtifactIssueCode::kIntegrityMismatch,
                  "digest mismatch was accepted");
  broken = encoded.artifact->bytes;
  write_u64(broken, 16, cloth::kMaximumArtifactMetadataSize + 1);
  expect_rejected(std::move(broken), cloth::ArtifactIssueCode::kLimitExceeded,
                  "oversized metadata declaration was accepted");
  broken = encoded.artifact->bytes;
  write_u64(broken, 24, cloth::kMaximumArtifactPayloadSize + 1);
  expect_rejected(std::move(broken), cloth::ArtifactIssueCode::kLimitExceeded,
                  "oversized payload declaration was accepted");
}

void metadata_canonicality_and_reference_failures(TestContext& test) {
  const auto encoded = cloth::write_package_artifact(make_artifact());
  if (!encoded.artifact) {
    test.expect(false, "metadata failure fixture could not be encoded");
    return;
  }
  const std::string original = metadata_text(encoded.artifact->bytes);
  const auto expect_rejected = [&](std::string metadata,
                                   std::string_view message) {
    const auto bytes =
        replace_metadata(encoded.artifact->bytes, std::move(metadata));
    test.expect(!cloth::read_package_artifact(bytes).is_valid(), message);
  };

  expect_rejected(" " + original, "insignificant JSON whitespace was accepted");
  std::string changed = original;
  changed.replace(changed.find("\"types\""), 7, "\"typos\"");
  expect_rejected(std::move(changed), "unknown metadata field was accepted");
  changed = original;
  changed.insert(changed.size() - 1, ",\"types\":[]");
  expect_rejected(std::move(changed), "duplicate metadata field was accepted");
  changed = original;
  changed.replace(changed.find("\"compiler_abi\":\"4\""), 18,
                  "\"compiler_abi\":4");
  expect_rejected(std::move(changed), "raw JSON integer was accepted");
  changed = original;
  changed.replace(changed.find("sample"), 1, "\\u0073");
  expect_rejected(std::move(changed),
                  "noncanonical Unicode escape was accepted");
  changed = original;
  const std::size_t owner = changed.find("\"owner\":\"") + 10;
  changed[owner] = changed[owner] == '0' ? '1' : '0';
  expect_rejected(std::move(changed),
                  "corrupted declaration owner was accepted");
  changed = original;
  changed.replace(changed.find("\"kind\":\"interface\""), 20,
                  "\"kind\":\"object\"");
  expect_rejected(std::move(changed), "payload-kind mismatch was accepted");

  std::string nested;
  nested.append(cloth::kMaximumArtifactNesting + 1, '[');
  nested += "null";
  nested.append(cloth::kMaximumArtifactNesting + 1, ']');
  expect_rejected(std::move(nested), "excessive JSON nesting was accepted");
  changed = original;
  changed[changed.find("sample")] = static_cast<char>(0xff);
  expect_rejected(std::move(changed), "invalid UTF-8 metadata was accepted");
}

void reference_map_limit_failures(TestContext& test) {
  auto oversized = make_artifact();
  oversized.imported.files[0].abi.descriptor->reference_offsets.resize(65'537,
                                                                       0);
  const auto rejected_model = cloth::write_package_artifact(oversized);
  test.expect(
      std::ranges::any_of(rejected_model.issues,
                          [](const auto& issue) {
                            return issue.code ==
                                   cloth::ArtifactIssueCode::kLimitExceeded;
                          }),
      "oversized descriptor model lost its limit diagnostic");
  const auto encoded = cloth::write_package_artifact(make_artifact());
  test.expect(encoded.is_valid(), "reference-map fixture did not encode");
  if (!encoded.artifact) return;
  const auto original = metadata_text(encoded.artifact->bytes);
  std::string excessive = "[\"0\"";
  for (std::size_t index = 1; index < 65'537; ++index) excessive += ",\"0\"";
  excessive += ']';
  for (const std::string_view section : {"\"descriptor\":", "\"types\":["}) {
    auto changed = original;
    const auto section_offset = changed.find(section);
    const std::string_view key = "\"reference_offsets\":";
    const auto key_offset = changed.find(key, section_offset);
    test.expect(key_offset != std::string::npos,
                "reference-map fixture is missing");
    if (key_offset == std::string::npos) continue;
    const auto begin = key_offset + key.size();
    const auto end = changed.find(']', begin);
    changed.replace(begin, end - begin + 1, excessive);
    const auto bytes =
        replace_metadata(encoded.artifact->bytes, std::move(changed));
    const auto rejected = cloth::read_package_artifact(bytes);
    test.expect(!rejected.is_valid() &&
                    std::ranges::any_of(
                        rejected.issues,
                        [](const auto& issue) {
                          return issue.code ==
                                 cloth::ArtifactIssueCode::kLimitExceeded;
                        }),
                "oversized reference map lost its limit diagnostic");
  }
}

void writer_rejects_invalid_models(TestContext& test) {
  auto artifact = make_artifact();
  artifact.sources.clear();
  test.expect(!cloth::write_package_artifact(artifact).is_valid(),
              "artifact without its source inventory was written");
  artifact = make_artifact();
  artifact.symbols[0].link_name = "not-canonical";
  test.expect(!cloth::write_package_artifact(artifact).is_valid(),
              "artifact with a corrupt Cloth symbol name was written");
  artifact = make_artifact();
  artifact.imported.files[0].abi.descriptor->parent_identity =
      artifact.imported.files[0].identity;
  test.expect(!cloth::write_package_artifact(artifact).is_valid(),
              "artifact with corrupt ABI ancestry was written");
  artifact = make_artifact(cloth::PackageArtifactKind::kObject);
  artifact.compatibility.native.reset();
  test.expect(!cloth::write_package_artifact(artifact).is_valid(),
              "object artifact without native compatibility was written");
}

void enum_metadata_failures(TestContext& test) {
  constexpr std::string_view source = "enum { Ready, ready, _Done }";
  cloth::Compilation compilation;
  compilation.add_package_source(
      cloth::SourceFile::from_memory("Status.co", std::string{source}),
      "sample", "", "1.0.0");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, "enum metadata fixture did not compile");
  if (!result.is_valid) return;
  const auto imported = cloth::build_imported_package_view(
      {"sample", "1.0.0"}, result.semantics, result.mir, result.abi);
  test.expect(imported.is_valid(), "enum metadata fixture did not export");
  if (!imported.view) return;
  const cloth::PackageArtifact artifact{
      cloth::PackageArtifactKind::kInterface,
      {cloth::kCompilerAbiVersion, cloth::kRuntimeAbiVersion,
       cloth::sha256("enum-schema-fixture"), result.abi.target, std::nullopt},
      {{"Status.co", cloth::sha256(source)}},
      {},
      *imported.view,
      {},
      {}};
  const auto encoded = cloth::write_package_artifact(artifact);
  test.expect(encoded.is_valid(), "enum metadata fixture did not encode");
  if (!encoded.artifact) return;
  const std::string original = metadata_text(encoded.artifact->bytes);
  const auto tag = original.find("\"tag\":\"0\"}");
  test.expect(tag != std::string::npos, "enum case schema lost its tag record");
  if (tag == std::string::npos) return;
  for (const std::string replacement :
       {"\"tag\":\"3\"}", "\"tag\":\"65536\"}", "\"tag\":\"00\"}",
        "\"tag\":\"-1\"}", "\"tag\":\"0\",\"visibility\":\"private\"}"}) {
    std::string changed = original;
    changed.replace(tag, std::string_view{"\"tag\":\"0\"}"}.size(),
                    replacement);
    const auto bytes =
        replace_metadata(encoded.artifact->bytes, std::move(changed));
    const auto decoded = cloth::read_package_artifact(bytes);
    test.expect(!decoded.is_valid() && !decoded.issues.empty(),
                "accepted malformed enum case schema: " + replacement);
  }
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"SHA-256 vectors", sha256_vectors},
      {"canonical interface round trip", canonical_interface_round_trip},
      {"object round trip and compatibility",
       object_round_trip_and_compatibility},
      {"envelope and integrity failures", envelope_and_integrity_failures},
      {"metadata canonicality and reference failures",
       metadata_canonicality_and_reference_failures},
      {"writer rejects invalid models", writer_rejects_invalid_models},
      {"enum metadata failures", enum_metadata_failures},
      {"reference-map limit failures", reference_map_limit_failures},
  };
  return cloth::test::run_tests(tests);
}
