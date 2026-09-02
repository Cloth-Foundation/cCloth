// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/package_pipeline.h"

#include "cloth/artifact/imported_package.h"
#include "cloth/artifact/package_artifact.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/compiler/shuttle_protocol.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/identity/canonical_identity.h"
#include "cloth/source/path.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cloth {
namespace {

struct LoadedArtifact {
  ShuttleV2ArtifactInput input;
  std::vector<std::uint8_t> bytes;
  PackageArtifact artifact;
};

struct EntrySelection {
  std::string mangled_name;
  bool returns_int32;
};

class PrivateOutputDirectory {
 public:
  explicit PrivateOutputDirectory(const std::filesystem::path& output) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 16; ++attempt) {
      path_ =
          output.parent_path() / (".cloth-package." + std::to_string(ticks) +
                                  "." + std::to_string(sequence.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        ready_ = true;
        return;
      }
      if (error) break;
    }
  }

  ~PrivateOutputDirectory() {
    if (ready_) {
      std::error_code ignored;
      static_cast<void>(std::filesystem::remove_all(path_, ignored));
    }
  }

  PrivateOutputDirectory(const PrivateOutputDirectory&) = delete;
  PrivateOutputDirectory& operator=(const PrivateOutputDirectory&) = delete;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
  bool ready_{false};
};

ShuttleV2ExecutionResult failure(int exit_code, std::string message) {
  if (!message.empty() && !message.ends_with('\n')) message.push_back('\n');
  return {exit_code, {}, std::move(message)};
}

ShuttleV2ExecutionResult cache_miss() { return {3, {}, {}}; }

std::string render_diagnostics(const DiagnosticEngine& diagnostics) {
  std::ostringstream output;
  for (const Diagnostic& diagnostic : diagnostics.diagnostics()) {
    const SourceLocation& location = diagnostic.range.begin;
    output << (location.file.empty() ? std::string_view{"<unknown>"}
                                     : location.file)
           << ':' << location.line << ':' << location.column << ": "
           << diagnostic_severity_name(diagnostic.severity) << ": "
           << diagnostic.message << '\n';
  }
  return output.str();
}

std::expected<std::vector<std::uint8_t>, std::string> read_file(
    const std::filesystem::path& path, std::uint64_t maximum_size,
    std::string_view description) {
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > maximum_size ||
      size > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::size_t>::max())) {
    return std::unexpected(std::string{description} +
                           " is unreadable or exceeds its size limit");
  }
  std::ifstream input{path, std::ios::binary};
  if (!input) {
    return std::unexpected("could not open " + std::string{description} + " '" +
                           path_to_utf8(path) + "'");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    return std::unexpected("could not read a stable " +
                           std::string{description} + " snapshot");
  }
  return bytes;
}

std::expected<ArtifactDigest, std::string> digest_file(
    const std::filesystem::path& path, std::string_view description) {
  auto bytes = read_file(path, kMaximumArtifactPayloadSize, description);
  if (!bytes) return std::unexpected(bytes.error());
  return sha256(*bytes);
}

std::expected<ArtifactCompatibility, std::string> compatibility(
    const TargetDataLayout& target, PackageArtifactKind kind,
    const std::filesystem::path& compiler_executable,
    const NativeToolchain& toolchain) {
  auto compiler_id = digest_file(compiler_executable, "compiler executable");
  if (!compiler_id) return std::unexpected(compiler_id.error());
  ArtifactCompatibility result{kCompilerAbiVersion, kRuntimeAbiVersion,
                               *compiler_id, target, std::nullopt};
  if (kind == PackageArtifactKind::kInterface) return result;

  auto runtime = digest_file(toolchain.runtime_library, "runtime archive");
  auto llc = digest_file(toolchain.llc, "LLVM llc");
  auto linker = digest_file(toolchain.linker, "native linker");
  if (!runtime) return std::unexpected(runtime.error());
  if (!llc) return std::unexpected(llc.error());
  if (!linker) return std::unexpected(linker.error());
  std::string object_format = "elf";
  if (toolchain.target_triple.contains("windows")) object_format = "coff";
  if (toolchain.target_triple.contains("apple")) object_format = "mach_o";
  result.native =
      ArtifactNativeCompatibility{toolchain.target_triple,
                                  std::move(object_format),
                                  toolchain.cpu,
                                  toolchain.features,
                                  toolchain.relocation_model,
                                  toolchain.code_model,
                                  *runtime,
                                  {{"linker", *linker}, {"llc", *llc}}};
  return result;
}

bool same_path(const std::filesystem::path& left,
               const std::filesystem::path& right) {
  if (left.lexically_normal() == right.lexically_normal()) return true;
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(left, right, error);
  return !error && equivalent;
}

std::expected<void, std::string> validate_output_aliases(
    const std::filesystem::path& output,
    std::span<const ShuttleV2ArtifactInput> artifacts,
    std::span<const ShuttleSourceInput> sources = {},
    std::span<const std::filesystem::path> protected_paths = {}) {
  for (const ShuttleV2ArtifactInput& artifact : artifacts) {
    if (same_path(output, artifact.path)) {
      return std::unexpected("output aliases artifact input '" +
                             path_to_utf8(artifact.path) + "'");
    }
  }
  for (const ShuttleSourceInput& source : sources) {
    if (same_path(output, source.source.path())) {
      return std::unexpected("output aliases source input '" +
                             path_to_utf8(source.source.path()) + "'");
    }
  }
  for (const std::filesystem::path& protected_path : protected_paths) {
    if (!protected_path.empty() && same_path(output, protected_path)) {
      return std::unexpected("output aliases selected compiler or tool '" +
                             path_to_utf8(protected_path) + "'");
    }
  }
  return {};
}

std::expected<void, std::string> write_file(
    const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
  std::ofstream output{path, std::ios::binary};
  if (!output) return std::unexpected("could not create staged output");
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  if (!output) return std::unexpected("could not write staged output");
  return {};
}

std::expected<void, std::string> publish_output(
    const std::filesystem::path& staged, const std::filesystem::path& output) {
  std::error_code error;
#if defined(_WIN32)
  if (MoveFileExW(staged.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING) ==
      FALSE) {
    error = std::error_code{static_cast<int>(GetLastError()),
                            std::system_category()};
  }
#else
  std::filesystem::rename(staged, output, error);
#endif
  if (error) {
    return std::unexpected("could not atomically publish output '" +
                           path_to_utf8(output) + "': " + error.message());
  }
  return {};
}

std::expected<std::vector<LoadedArtifact>, std::string> load_artifacts(
    std::span<const ShuttleV2ArtifactInput> inputs,
    const std::optional<ArtifactCompatibility>& expected_compatibility,
    std::optional<PackageArtifactKind> expected_kind) {
  std::vector<LoadedArtifact> loaded;
  loaded.reserve(inputs.size());
  constexpr std::uint64_t kMaximumFileSize =
      64 + kMaximumArtifactMetadataSize + kMaximumArtifactPayloadSize;
  for (const ShuttleV2ArtifactInput& input : inputs) {
    auto bytes = read_file(input.path, kMaximumFileSize, "package artifact");
    if (!bytes) return std::unexpected(bytes.error());
    auto decoded = read_package_artifact(*bytes, expected_compatibility);
    if (!decoded.is_valid()) {
      const std::string message = decoded.issues.empty()
                                      ? "artifact is invalid"
                                      : decoded.issues.front().message;
      return std::unexpected("artifact '" + path_to_utf8(input.path) +
                             "' for package '" + input.package.name +
                             "' is invalid: " + message);
    }
    if (*decoded.digest != input.digest ||
        decoded.artifact->imported.package != input.package) {
      return std::unexpected(
          "artifact input identity or digest mismatch for '" +
          input.package.name + "'");
    }
    if (expected_kind && decoded.artifact->kind != *expected_kind) {
      return std::unexpected("artifact kind mismatch for package '" +
                             input.package.name + "'");
    }
    loaded.push_back({input, std::move(*bytes), std::move(*decoded.artifact)});
  }
  return loaded;
}

std::expected<LoadedArtifact, std::string> load_candidate(
    const std::filesystem::path& path,
    const ArtifactCompatibility& expected_compatibility,
    PackageArtifactKind expected_kind, const PackageIdentity& package) {
  constexpr std::uint64_t kMaximumFileSize =
      64 + kMaximumArtifactMetadataSize + kMaximumArtifactPayloadSize;
  auto bytes = read_file(path, kMaximumFileSize, "candidate package artifact");
  if (!bytes) return std::unexpected(bytes.error());
  auto decoded = read_package_artifact(*bytes, expected_compatibility);
  if (!decoded.is_valid() || decoded.artifact->kind != expected_kind ||
      decoded.artifact->imported.package != package) {
    return std::unexpected("candidate package artifact is not reusable");
  }
  ShuttleV2ArtifactInput input{package, *decoded.digest, path};
  return LoadedArtifact{std::move(input), std::move(*bytes),
                        std::move(*decoded.artifact)};
}

std::expected<std::vector<ArtifactSource>, std::string> source_inventory(
    std::span<const ShuttleSourceInput> sources,
    const std::filesystem::path& source_root) {
  std::vector<ArtifactSource> inventory;
  inventory.reserve(sources.size());
  for (const ShuttleSourceInput& source : sources) {
    std::error_code error;
    std::filesystem::path relative =
        std::filesystem::relative(source.source.path(), source_root, error);
    if (error) {
      return std::unexpected("could not derive logical source path");
    }
    std::string logical = path_to_utf8(relative);
    std::ranges::replace(logical, '\\', '/');
    inventory.push_back({std::move(logical), sha256(source.source.contents())});
  }
  std::ranges::sort(inventory, {}, &ArtifactSource::path);
  return inventory;
}

std::expected<std::vector<ArtifactDependency>, std::string>
dependency_inventory(std::span<const ShuttleV2DependencyInput> dependencies,
                     std::span<const LoadedArtifact> artifacts) {
  std::map<std::string, const LoadedArtifact*, std::less<>> by_name;
  for (const LoadedArtifact& artifact : artifacts) {
    by_name.emplace(artifact.input.package.name, &artifact);
  }
  std::vector<ArtifactDependency> inventory;
  inventory.reserve(dependencies.size());
  for (const ShuttleV2DependencyInput& dependency : dependencies) {
    const auto found = by_name.find(dependency.package);
    if (found == by_name.end()) {
      return std::unexpected("direct dependency is missing");
    }
    inventory.push_back({dependency.alias, found->second->input.package,
                         found->second->input.digest});
  }
  std::ranges::sort(inventory, {}, &ArtifactDependency::alias);
  return inventory;
}

std::expected<void, std::string> validate_closure(
    std::span<const LoadedArtifact> loaded,
    std::span<const std::string> roots) {
  std::map<std::string, const LoadedArtifact*, std::less<>> packages;
  for (const LoadedArtifact& artifact : loaded) {
    packages.emplace(artifact.input.package.name, &artifact);
  }
  std::set<std::string, std::less<>> reachable;
  std::vector<std::string> pending(roots.begin(), roots.end());
  while (!pending.empty()) {
    std::string package = std::move(pending.back());
    pending.pop_back();
    if (!reachable.insert(package).second) continue;
    const auto current = packages.find(package);
    if (current == packages.end()) {
      return std::unexpected("dependency closure is missing package '" +
                             package + "'");
    }
    for (const ArtifactDependency& dependency :
         current->second->artifact.dependencies) {
      const auto target = packages.find(dependency.package.name);
      if (target == packages.end() ||
          target->second->input.package != dependency.package ||
          target->second->input.digest != dependency.digest) {
        return std::unexpected("package '" + package +
                               "' has an unsatisfied dependency edge to '" +
                               dependency.package.name + "'");
      }
      pending.push_back(dependency.package.name);
    }
  }
  if (reachable.size() != loaded.size()) {
    return std::unexpected("artifact closure contains an unreachable package");
  }

  std::map<std::string, std::size_t, std::less<>> indegrees;
  std::map<std::string, std::vector<std::string>, std::less<>> dependents;
  for (const auto& [name, artifact] : packages) {
    indegrees.emplace(name, artifact->artifact.dependencies.size());
    for (const ArtifactDependency& dependency :
         artifact->artifact.dependencies) {
      dependents[dependency.package.name].push_back(name);
    }
  }
  std::set<std::string, std::less<>> ready;
  for (const auto& [name, degree] : indegrees) {
    if (degree == 0) ready.insert(name);
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const std::string name = *ready.begin();
    ready.erase(ready.begin());
    ++visited;
    for (const std::string& dependent : dependents[name]) {
      if (--indegrees[dependent] == 0) ready.insert(dependent);
    }
  }
  if (visited != loaded.size()) {
    return std::unexpected("artifact dependency closure contains a cycle");
  }
  std::vector<const ImportedPackageView*> views;
  for (const auto& artifact : loaded)
    views.push_back(&artifact.artifact.imported);
  const auto issues = verify_imported_package_closure(views);
  if (!issues.empty()) {
    return std::unexpected(issues.front().record + ": " +
                           issues.front().message);
  }
  return {};
}

void add_owned_symbols(
    const ImportedPackageView& imported,
    std::map<std::string, ArtifactSymbol, std::less<>>& out) {
  for (const ImportedFile& file : imported.files) {
    std::set<std::string, std::less<>> abstract;
    for (const ImportedMember& member : file.members) {
      if (member.is_abstract) abstract.insert(member.identity);
    }
    if (file.abi.descriptor && !file.abi.descriptor->mangled_name.empty()) {
      out.emplace(file.abi.descriptor->mangled_name,
                  ArtifactSymbol{file.abi.descriptor->mangled_name,
                                 file.abi.descriptor->identity,
                                 ArtifactSymbolRole::kDefinition,
                                 ArtifactSymbolKind::kDescriptor,
                                 "descriptor:file_class"});
    }
    for (const ImportedStaticFieldAbi& field : file.abi.static_fields) {
      if (field.linkage != AbiLinkage::kExternal) continue;
      out.emplace(
          field.mangled_name,
          ArtifactSymbol{
              field.mangled_name, field.member_identity,
              ArtifactSymbolRole::kDefinition, ArtifactSymbolKind::kStaticField,
              "global:" + mangle_canonical_identity(field.type_identity)});
    }
    for (const ImportedCallableAbi& callable : file.abi.callables) {
      const std::string signature = imported_callable_signature(
          callable.return_mode, callable.receiver_mode,
          callable.return_type_identity, callable.parameters);
      if (callable.linkage == AbiLinkage::kExternal &&
          !abstract.contains(callable.member_identity)) {
        out.emplace(
            callable.mangled_name,
            ArtifactSymbol{callable.mangled_name, callable.member_identity,
                           ArtifactSymbolRole::kDefinition,
                           ArtifactSymbolKind::kCallable, signature});
      }
      if (callable.initializer_identity &&
          callable.initializer_linkage == AbiLinkage::kExternal) {
        out.emplace(
            callable.initializer_mangled_name,
            ArtifactSymbol{callable.initializer_mangled_name,
                           *callable.initializer_identity,
                           ArtifactSymbolRole::kDefinition,
                           ArtifactSymbolKind::kConstructorInitializer,
                           imported_callable_signature(
                               callable.initializer_return_mode,
                               callable.initializer_receiver_mode,
                               *callable.initializer_return_type_identity,
                               callable.initializer_parameters)});
      }
    }
  }
}

std::vector<ArtifactSymbol> artifact_symbols(
    const ImportedPackageView& imported,
    std::span<const LoadedArtifact> dependencies, PackageArtifactKind kind) {
  std::map<std::string, ArtifactSymbol, std::less<>> symbols;
  add_owned_symbols(imported, symbols);
  for (const LoadedArtifact& dependency : dependencies) {
    for (const ArtifactSymbol& symbol : dependency.artifact.symbols) {
      if (symbol.role != ArtifactSymbolRole::kDefinition) continue;
      ArtifactSymbol requirement = symbol;
      requirement.role = ArtifactSymbolRole::kRequirement;
      symbols.try_emplace(requirement.link_name, std::move(requirement));
    }
  }
  if (kind == PackageArtifactKind::kObject) {
    symbols.try_emplace(
        "cloth_rt_alloc",
        ArtifactSymbol{"cloth_rt_alloc", std::nullopt,
                       ArtifactSymbolRole::kRequirement,
                       ArtifactSymbolKind::kRuntime, "c:ptr(ptr)"});
    symbols.try_emplace(
        "cloth_rt_array_alloc",
        ArtifactSymbol{"cloth_rt_array_alloc", std::nullopt,
                       ArtifactSymbolRole::kRequirement,
                       ArtifactSymbolKind::kRuntime, "c:ptr(i32,ptr)"});
  }
  std::vector<ArtifactSymbol> result;
  result.reserve(symbols.size());
  for (auto& [name, symbol] : symbols) result.push_back(std::move(symbol));
  return result;
}

std::expected<EntrySelection, std::string> select_entry(
    const PackageArtifact& root, std::string_view entry) {
  const auto file = std::ranges::find(root.imported.files, entry,
                                      &ImportedFile::logical_path);
  if (file == root.imported.files.end()) {
    return std::unexpected("entry path does not name a root package file");
  }
  const std::string void_type = canonical_primitive_identity("void");
  const std::string int32_type = canonical_primitive_identity("int32");
  std::optional<EntrySelection> selected;
  bool saw_main = false;
  for (const ImportedMember& member : file->members) {
    if (member.name != "Main" || member.kind != ImportedMemberKind::kFunction) {
      continue;
    }
    saw_main = true;
    if (member.visibility != Visibility::kPublic || !member.is_static ||
        !member.parameters.empty() ||
        (member.type_identity != void_type &&
         member.type_identity != int32_type)) {
      continue;
    }
    const auto callable =
        std::ranges::find(file->abi.callables, member.identity,
                          &ImportedCallableAbi::member_identity);
    if (callable == file->abi.callables.end() ||
        callable->linkage != AbiLinkage::kExternal ||
        !callable->parameters.empty()) {
      continue;
    }
    if (selected) {
      return std::unexpected(
          "native program has more than one eligible 'Main' function");
    }
    selected = EntrySelection{callable->mangled_name,
                              member.type_identity == int32_type};
  }
  if (selected) return *selected;
  return std::unexpected(
      saw_main ? "entry point 'Main' must be public and static, take no "
                 "parameters, and return no value or int32"
               : "native program requires a public static 'Main' function");
}

LlvmIrModule entry_wrapper(const EntrySelection& entry,
                           const ArtifactCompatibility& compatibility) {
  const std::string return_type = entry.returns_int32 ? "i32" : "void";
  std::ostringstream output;
  output << "; Cloth entry wrapper\nsource_filename = \"cloth-entry\"\n"
         << "target datalayout = \"" << compatibility.target.llvm_data_layout
         << "\"\n"
         << "target triple = \"" << compatibility.native->target_triple
         << "\"\n\ndeclare " << return_type << " @" << entry.mangled_name
         << "()\n\ndefine i32 @main() {\nentry:\n";
  if (entry.returns_int32) {
    output << "  %result = call i32 @" << entry.mangled_name
           << "()\n  ret i32 %result\n";
  } else {
    output << "  call void @" << entry.mangled_name << "()\n  ret i32 0\n";
  }
  output << "}\n";
  return {output.str()};
}

std::expected<void, std::string> validate_link_symbols(
    std::span<const LoadedArtifact> artifacts) {
  std::map<std::string, const ArtifactSymbol*, std::less<>> definitions;
  for (const LoadedArtifact& artifact : artifacts) {
    for (const ArtifactSymbol& symbol : artifact.artifact.symbols) {
      if (symbol.role != ArtifactSymbolRole::kDefinition) continue;
      const auto [previous, inserted] =
          definitions.emplace(symbol.link_name, &symbol);
      if (!inserted) {
        return std::unexpected("duplicate owning definition '" +
                               symbol.link_name + "'");
      }
    }
  }
  for (const LoadedArtifact& artifact : artifacts) {
    for (const ArtifactSymbol& requirement : artifact.artifact.symbols) {
      if (requirement.role != ArtifactSymbolRole::kRequirement ||
          requirement.kind == ArtifactSymbolKind::kRuntime) {
        continue;
      }
      const auto definition = definitions.find(requirement.link_name);
      if (definition == definitions.end() ||
          definition->second->canonical_identity !=
              requirement.canonical_identity ||
          definition->second->kind != requirement.kind ||
          definition->second->abi_signature != requirement.abi_signature) {
        return std::unexpected("unresolved or incompatible Cloth symbol '" +
                               requirement.link_name + "'");
      }
    }
  }
  return {};
}

ShuttleV2ExecutionResult execute_compile(
    const ShuttleV2CompileRequest& request,
    const std::filesystem::path& compiler_executable,
    const NativeToolchain& toolchain) {
  auto expected = compatibility(request.target, request.artifact_kind,
                                compiler_executable, toolchain);
  if (!expected) return failure(2, "clothc: error: " + expected.error());
  auto dependencies =
      load_artifacts(request.artifacts, *expected, request.artifact_kind);
  if (!dependencies) {
    return failure(2, "clothc: error: " + dependencies.error());
  }
  std::vector<std::string> closure_roots;
  std::set<std::string, std::less<>> direct_packages;
  for (const ShuttleV2DependencyInput& dependency : request.dependencies) {
    closure_roots.push_back(dependency.package);
    direct_packages.insert(dependency.package);
  }
  if (auto valid = validate_closure(*dependencies, closure_roots); !valid) {
    return failure(2, "clothc: error: " + valid.error());
  }
  if (direct_packages.size() != closure_roots.size()) {
    return failure(2,
                   "clothc: error: multiple aliases target the same package");
  }

  ShuttleBuildRequest source_request{
      request.target,
      ShuttleOutputKind::kCheck,
      std::nullopt,
      request.package.name,
      request.entry ? std::optional<std::filesystem::path>{*request.entry}
                    : std::nullopt,
      {{request.package.name, request.package.version, request.source_root}},
      {}};
  for (const ShuttleV2DependencyInput& dependency : request.dependencies) {
    source_request.dependencies.push_back(
        {request.package.name, dependency.alias, dependency.package});
  }
  auto sources = load_shuttle_sources(source_request);
  if (!sources) return failure(2, "clothc: error: " + sources.error());
  auto entry = resolve_shuttle_entry(source_request, *sources);
  if (!entry) return failure(2, "clothc: error: " + entry.error());
  auto artifact_sources = source_inventory(*sources, request.source_root);
  if (!artifact_sources) {
    return failure(2, "clothc: error: " + artifact_sources.error());
  }
  std::array<std::filesystem::path, 4> protected_paths{
      compiler_executable, {}, {}, {}};
  if (request.artifact_kind == PackageArtifactKind::kObject) {
    protected_paths[1] = toolchain.llc;
    protected_paths[2] = toolchain.linker;
    protected_paths[3] = toolchain.runtime_library;
  }
  if (auto valid = validate_output_aliases(request.output, request.artifacts,
                                           *sources, protected_paths);
      !valid) {
    return failure(2, "clothc: error: " + valid.error());
  }

  Compilation compilation{request.target};
  std::vector<CompilationDependency> semantic_dependencies;
  for (const ShuttleV2DependencyInput& dependency : request.dependencies) {
    semantic_dependencies.push_back(
        {request.package.name, dependency.alias, dependency.package});
  }
  compilation.set_package_dependencies(std::move(semantic_dependencies));
  for (const LoadedArtifact& dependency : *dependencies) {
    compilation.add_imported_package(dependency.artifact.imported);
  }
  for (ShuttleSourceInput& source : *sources) {
    compilation.add_package_source(
        std::move(source.source), std::move(source.package),
        std::move(source.source_package), std::move(source.version));
  }
  DiagnosticEngine diagnostics;
  CompilationResult result = compilation.analyze(diagnostics);
  if (result.is_valid && *entry) {
    static_cast<void>(find_native_entry_point(result.abi, result.semantics,
                                              diagnostics, **entry));
  }
  if (!result.is_valid || diagnostics.has_errors()) {
    return {1, {}, render_diagnostics(diagnostics)};
  }
  auto imported = build_imported_package_view(request.package, result.semantics,
                                              result.mir, result.abi);
  if (!imported.is_valid()) {
    const std::string message = imported.issues.empty()
                                    ? "could not build imported package view"
                                    : imported.issues.front().message;
    return failure(2, "clothc: error: " + message);
  }

  auto direct_dependencies =
      dependency_inventory(request.dependencies, *dependencies);
  if (!direct_dependencies) {
    return failure(2, "clothc: error: " + direct_dependencies.error());
  }

  std::vector<std::uint8_t> native_payload;
  const PrivateOutputDirectory staged{request.output};
  if (!staged.ready()) {
    return failure(2, "clothc: error: could not create private output staging");
  }
  if (request.artifact_kind == PackageArtifactKind::kObject) {
    LlvmIrOptions options;
    options.package = request.package;
    auto llvm = emit_llvm_ir(result.mir, result.abi, result.semantics,
                             diagnostics, options);
    if (!llvm || diagnostics.has_errors()) {
      return {2, {}, render_diagnostics(diagnostics)};
    }
#if defined(_WIN32)
    const auto object_path = staged.path() / "package.obj";
#else
    const auto object_path = staged.path() / "package.o";
#endif
    if (auto built = build_native_object(*llvm, object_path, toolchain);
        !built) {
      return failure(2, "clothc: error: " + built.error().message);
    }
    auto bytes = read_file(object_path, kMaximumArtifactPayloadSize,
                           "native package object");
    if (!bytes) return failure(2, "clothc: error: " + bytes.error());
    native_payload = std::move(*bytes);
  }
  PackageArtifact artifact{
      request.artifact_kind,        std::move(*expected),
      std::move(*artifact_sources), std::move(*direct_dependencies),
      std::move(*imported.view),    {},
      std::move(native_payload)};
  artifact.symbols =
      artifact_symbols(artifact.imported, *dependencies, request.artifact_kind);
  auto encoded = write_package_artifact(artifact);
  if (!encoded.is_valid()) {
    const std::string message = encoded.issues.empty()
                                    ? "could not encode package artifact"
                                    : encoded.issues.front().message;
    return failure(2, "clothc: error: " + message);
  }
  const std::string receipt =
      shuttle_artifact_receipt_json(artifact, encoded.artifact->digest) + '\n';
  if (receipt.size() > 16ULL * 1024 * 1024) {
    return failure(2, "clothc: error: artifact receipt exceeds 16 MiB");
  }
  const auto staged_artifact = staged.path() / "completed.cpa";
  if (auto written = write_file(staged_artifact, encoded.artifact->bytes);
      !written) {
    return failure(2, "clothc: error: " + written.error());
  }
  if (auto published = publish_output(staged_artifact, request.output);
      !published) {
    return failure(2, "clothc: error: " + published.error());
  }
  return {0, receipt, {}};
}

ShuttleV2ExecutionResult execute_inspect(
    const ShuttleV2InspectRequest& request) {
  constexpr std::uint64_t kMaximumFileSize =
      64 + kMaximumArtifactMetadataSize + kMaximumArtifactPayloadSize;
  auto bytes = read_file(request.input, kMaximumFileSize, "package artifact");
  if (!bytes) return failure(2, "clothc: error: " + bytes.error());
  auto decoded = read_package_artifact(*bytes);
  if (!decoded.is_valid()) {
    const std::string message = decoded.issues.empty()
                                    ? "artifact is invalid"
                                    : decoded.issues.front().message;
    return failure(2, "clothc: error: artifact '" +
                          path_to_utf8(request.input) +
                          "' is invalid: " + message);
  }
  return {
      0,
      shuttle_artifact_receipt_json(*decoded.artifact, *decoded.digest) + '\n',
      {}};
}

ShuttleV2ExecutionResult execute_reuse(
    const ShuttleV2ReuseRequest& request,
    const std::filesystem::path& compiler_executable,
    const NativeToolchain& toolchain) {
  auto expected = compatibility(request.target, request.artifact_kind,
                                compiler_executable, toolchain);
  if (!expected) return failure(2, "clothc: error: " + expected.error());
  auto candidate = load_candidate(request.input, *expected,
                                  request.artifact_kind, request.package);
  if (!candidate) return cache_miss();

  auto dependencies =
      load_artifacts(request.artifacts, *expected, request.artifact_kind);
  if (!dependencies) {
    return failure(2, "clothc: error: " + dependencies.error());
  }
  std::vector<std::string> closure_roots;
  std::set<std::string, std::less<>> direct_packages;
  for (const ShuttleV2DependencyInput& dependency : request.dependencies) {
    closure_roots.push_back(dependency.package);
    direct_packages.insert(dependency.package);
  }
  if (auto valid = validate_closure(*dependencies, closure_roots); !valid) {
    return failure(2, "clothc: error: " + valid.error());
  }
  if (direct_packages.size() != closure_roots.size()) {
    return failure(2,
                   "clothc: error: multiple aliases target the same package");
  }
  auto direct_dependencies =
      dependency_inventory(request.dependencies, *dependencies);
  if (!direct_dependencies) {
    return failure(2, "clothc: error: " + direct_dependencies.error());
  }
  if (candidate->artifact.dependencies != *direct_dependencies) {
    return cache_miss();
  }

  ShuttleBuildRequest source_request{
      request.target,
      ShuttleOutputKind::kCheck,
      std::nullopt,
      request.package.name,
      request.entry ? std::optional<std::filesystem::path>{*request.entry}
                    : std::nullopt,
      {{request.package.name, request.package.version, request.source_root}},
      {}};
  for (const ShuttleV2DependencyInput& dependency : request.dependencies) {
    source_request.dependencies.push_back(
        {request.package.name, dependency.alias, dependency.package});
  }
  auto sources = load_shuttle_sources(source_request);
  if (!sources) return failure(2, "clothc: error: " + sources.error());
  auto current_sources = source_inventory(*sources, request.source_root);
  if (!current_sources) {
    return failure(2, "clothc: error: " + current_sources.error());
  }
  if (candidate->artifact.sources != *current_sources) return cache_miss();
  if (request.entry && !select_entry(candidate->artifact, *request.entry)) {
    return cache_miss();
  }

  return {0,
          shuttle_artifact_receipt_json(candidate->artifact,
                                        candidate->input.digest) +
              '\n',
          {}};
}

ShuttleV2ExecutionResult execute_link(
    const ShuttleV2LinkRequest& request,
    const std::filesystem::path& compiler_executable,
    const NativeToolchain& toolchain) {
  auto expected = compatibility(request.target, PackageArtifactKind::kObject,
                                compiler_executable, toolchain);
  if (!expected) return failure(2, "clothc: error: " + expected.error());
  auto artifacts = load_artifacts(request.artifacts, *expected,
                                  PackageArtifactKind::kObject);
  if (!artifacts) return failure(2, "clothc: error: " + artifacts.error());
  const std::array closure_roots{request.root_package};
  if (auto valid = validate_closure(*artifacts, closure_roots); !valid) {
    return failure(2, "clothc: error: " + valid.error());
  }
  if (auto valid = validate_link_symbols(*artifacts); !valid) {
    return failure(2, "clothc: error: " + valid.error());
  }
  const std::array protected_paths{compiler_executable, toolchain.llc,
                                   toolchain.linker, toolchain.runtime_library};
  if (auto valid = validate_output_aliases(request.output, request.artifacts,
                                           {}, protected_paths);
      !valid) {
    return failure(2, "clothc: error: " + valid.error());
  }
  const auto root = std::ranges::find_if(*artifacts, [&](const auto& artifact) {
    return artifact.input.package.name == request.root_package;
  });
  if (root == artifacts->end()) {
    return failure(2, "clothc: error: root artifact is missing");
  }
  auto entry = select_entry(root->artifact, request.entry);
  if (!entry) return failure(1, "clothc: error: " + entry.error());

  const PrivateOutputDirectory staged{request.output};
  if (!staged.ready()) {
    return failure(2, "clothc: error: could not create private output staging");
  }
  std::vector<std::filesystem::path> object_paths;
  object_paths.reserve(artifacts->size() + 1);
  for (std::size_t index = 0; index < artifacts->size(); ++index) {
#if defined(_WIN32)
    const auto path =
        staged.path() / ("package-" + std::to_string(index) + ".obj");
#else
    const auto path =
        staged.path() / ("package-" + std::to_string(index) + ".o");
#endif
    if (auto written =
            write_file(path, (*artifacts)[index].artifact.native_payload);
        !written) {
      return failure(2, "clothc: error: " + written.error());
    }
    object_paths.push_back(path);
  }
#if defined(_WIN32)
  const auto wrapper_path = staged.path() / "entry.obj";
#else
  const auto wrapper_path = staged.path() / "entry.o";
#endif
  if (auto built = build_native_object(entry_wrapper(*entry, *expected),
                                       wrapper_path, toolchain);
      !built) {
    return failure(2, "clothc: error: " + built.error().message);
  }
  object_paths.push_back(wrapper_path);
  const auto executable = staged.path() / "completed";
  if (auto linked = link_native_objects(object_paths, executable, toolchain);
      !linked) {
    return failure(2, "clothc: error: " + linked.error().message);
  }
  if (auto published = publish_output(executable, request.output); !published) {
    return failure(2, "clothc: error: " + published.error());
  }
  return {0, {}, {}};
}

}  // namespace

ShuttleV2ExecutionResult execute_shuttle_v2_request(
    const ShuttleV2Request& request,
    const std::filesystem::path& compiler_executable,
    const NativeToolchain& toolchain) {
  if (const auto* compile = std::get_if<ShuttleV2CompileRequest>(&request)) {
    return execute_compile(*compile, compiler_executable, toolchain);
  }
  if (const auto* inspect = std::get_if<ShuttleV2InspectRequest>(&request)) {
    return execute_inspect(*inspect);
  }
  if (const auto* reuse = std::get_if<ShuttleV2ReuseRequest>(&request)) {
    return execute_reuse(*reuse, compiler_executable, toolchain);
  }
  return execute_link(std::get<ShuttleV2LinkRequest>(request),
                      compiler_executable, toolchain);
}

}  // namespace cloth
