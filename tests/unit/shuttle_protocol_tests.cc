// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/compilation.h"
#include "cloth/compiler/shuttle_protocol.h"
#include "cloth/compiler/shuttle_protocol_v2.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"

#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto identifier =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("cloth-shuttle-protocol-" + std::to_string(identifier));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output{path, std::ios::binary};
  output << contents;
}

std::vector<std::filesystem::path> valid_arguments(
    const std::filesystem::path& app, const std::filesystem::path& models) {
  return {"--shuttle-protocol",
          "1",
          "--target",
          "wasm32",
          "--output-kind",
          "check",
          "--root-package",
          "app",
          "--entry",
          "Main.co",
          "--package",
          "models",
          "0.1.0",
          models,
          "--package",
          "app",
          "1.0.0",
          app,
          "--dependency",
          "app",
          "models",
          "models"};
}

void valid_protocol_plan(TestContext& test) {
  TemporaryDirectory temporary;
  const std::filesystem::path app = temporary.path() / "app";
  const std::filesystem::path models = temporary.path() / "models";
  write_file(app / "Main.co", "import models::User;\nstatic func Main() {}\n");
  write_file(models / "User.co", "User() {}\n");

  const auto plan = cloth::prepare_shuttle_build(valid_arguments(app, models));
  test.expect(plan.has_value(), "valid Shuttle build plan was rejected");
  if (!plan) {
    return;
  }
  test.expect(plan->request.packages.size() == 2,
              "protocol package records were lost");
  test.expect(plan->request.packages[0].name == "app" &&
                  plan->request.packages[1].name == "models",
              "package records were not ordered by name");
  test.expect(plan->sources.size() == 2,
              "package source enumeration has the wrong size");
  test.expect(plan->entry_file == "app.Main",
              "entry file identity is not package-qualified");
}

void package_dependency_import(TestContext& test) {
  TemporaryDirectory temporary;
  const std::filesystem::path app = temporary.path() / "app";
  const std::filesystem::path models = temporary.path() / "models";
  write_file(app / "Main.co",
             "import models::User;\n"
             "static func Main() {\n"
             "  User value = User();\n"
             "  print(value.Name());\n"
             "}\n");
  write_file(models / "User.co",
             "User() {}\n"
             "func Name(): string { return \"Cloth\"; }\n");

  auto plan = cloth::prepare_shuttle_build(valid_arguments(app, models));
  test.expect(plan.has_value(), "dependency import plan was rejected");
  if (!plan) {
    return;
  }
  cloth::Compilation compilation{plan->request.target};
  compilation.set_package_dependencies(
      {cloth::CompilationDependency{"app", "models", "models"}});
  for (cloth::ShuttleSourceInput& source : plan->sources) {
    compilation.add_package_source(
        std::move(source.source), std::move(source.package),
        std::move(source.source_package), std::move(source.version));
  }
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);
  test.expect(result.is_valid && !diagnostics.has_errors(),
              "dependency-alias import did not resolve");

  bool retained_package_identity = false;
  for (const cloth::SemanticType& type : result.semantics.types()) {
    retained_package_identity =
        retained_package_identity || type.name == "models.User";
  }
  test.expect(retained_package_identity,
              "dependency type lost its Shuttle package identity");
}

void invalid_protocol_graphs(TestContext& test) {
  TemporaryDirectory temporary;
  const std::filesystem::path app = temporary.path() / "app";
  const std::filesystem::path models = temporary.path() / "models";
  write_file(app / "Main.co", "static func Main() {}\n");
  write_file(models / "User.co", "User() {}\n");

  std::vector<std::filesystem::path> cycle = valid_arguments(app, models);
  cycle.insert(cycle.end(), {"--dependency", "models", "app", "app"});
  const auto cycle_plan = cloth::prepare_shuttle_build(cycle);
  test.expect(
      !cycle_plan && cycle_plan.error().find("cycle") != std::string::npos,
      "protocol dependency cycle was accepted");

  write_file(app / "models" / "Local.co", "Local() {}\n");
  const auto collision =
      cloth::prepare_shuttle_build(valid_arguments(app, models));
  test.expect(!collision && collision.error().find(
                                "collides with a local source package") !=
                                std::string::npos,
              "dependency alias and local package collision was accepted");
}

void expect_rejected(TestContext& test,
                     const std::vector<std::filesystem::path>& arguments) {
  try {
    test.expect(!cloth::prepare_shuttle_build(arguments),
                "invalid protocol request was accepted");
  } catch (const std::exception& error) {
    test.expect(false,
                std::string{"invalid protocol request threw: "} + error.what());
  }
}

void protocol_configuration_validation(TestContext& test) {
  TemporaryDirectory temporary;
  const auto app = temporary.path() / "app";
  const auto models = temporary.path() / "models";
  write_file(app / "Main.co", "static func Main() {}\n");
  write_file(app / "nested" / "Main.co", "static func Main() {}\n");
  write_file(models / "User.co", "User() {}\n");
  const auto baseline = valid_arguments(app, models);

  for (const std::string_view version :
       {"", "1", "1.0", "1.0.0.1", "01.0.0", "1.0.0-01", "1.0.0+"}) {
    auto arguments = baseline;
    arguments[12] = version;
    expect_rejected(test, arguments);
  }
  for (const std::string_view version :
       {"0.0.0", "1.2.3-alpha.1+build.01", "1.0.0-0"}) {
    auto arguments = baseline;
    arguments[12] = version;
    test.expect(cloth::prepare_shuttle_build(arguments).has_value(),
                "valid semantic version was rejected");
  }
  for (const std::filesystem::path& entry :
       {std::filesystem::path{"../Main.co"},
        {"./Main.co"},
        {"nested/./Main.co"},
        {"nested\\Main.co"},
        {"Main.CO"}}) {
    auto arguments = baseline;
    arguments[9] = entry;
    expect_rejected(test, arguments);
  }
  for (const auto& extra : std::vector<std::vector<std::filesystem::path>>{
           {"--target", "wasm32"},
           {"--unknown"},
           {"--package", "short"},
           {"--dependency", "app", "for", "models"},
           {"--dependency", "app", "other", "models"},
           {"--dependency", "app", "missing", "missing"},
           {"--package", "app", "0.1.0", models}}) {
    auto arguments = baseline;
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    expect_rejected(test, arguments);
  }
}

void output_configuration_validation(TestContext& test) {
  TemporaryDirectory temporary;
  const auto app = temporary.path() / "app";
  const auto models = temporary.path() / "models";
  write_file(app / "Main.co", "static func Main() {}\n");
  write_file(models / "User.co", "User() {}\n");
  auto arguments = valid_arguments(app, models);
  arguments.insert(arguments.end(), {"--output", temporary.path() / "out.ll"});
  expect_rejected(test, arguments);  // check never accepts output.
  arguments[5] = "llvm-ir";
  test.expect(cloth::prepare_shuttle_build(arguments).has_value(),
              "LLVM IR output was rejected");
  arguments.back() = "relative.ll";
  expect_rejected(test, arguments);
  arguments.back() = temporary.path() / "missing" / "out.ll";
  expect_rejected(test, arguments);
  arguments = valid_arguments(app, models);
  arguments[5] = "executable";
  arguments.insert(arguments.end(), {"--output", temporary.path() / "app.exe"});
  expect_rejected(test, arguments);  // wasm32 has no native linker.
  arguments[3] = "x86_64";
  arguments.erase(arguments.begin() + 8, arguments.begin() + 10);
  expect_rejected(test, arguments);  // Executables require an entry.
}

void protocol_v2_operations(TestContext& test) {
  TemporaryDirectory temporary;
  const auto source_root = temporary.path() / "src";
  const auto artifact_path = temporary.path() / "dependency.cpa";
  write_file(source_root / "Main.co", "static func Main() {}\n");
  write_file(artifact_path, "fixture");
  const std::string digest(64, '0');

  const std::vector<std::filesystem::path> compile{"--shuttle-protocol",
                                                   "2",
                                                   "--operation",
                                                   "compile",
                                                   "--target",
                                                   "wasm32",
                                                   "--artifact-kind",
                                                   "interface",
                                                   "--output",
                                                   temporary.path() / "app.cpa",
                                                   "--package",
                                                   "app",
                                                   "1.2.3",
                                                   source_root,
                                                   "--entry",
                                                   "Main.co"};
  auto request = cloth::prepare_shuttle_v2_request(compile);
  test.expect(request && std::holds_alternative<cloth::ShuttleV2CompileRequest>(
                             *request),
              "valid protocol-v2 compile request was rejected");

  const std::array<std::filesystem::path, 6> inspect{
      "--shuttle-protocol", "2", "--operation", "inspect", "--input",
      artifact_path};
  request = cloth::prepare_shuttle_v2_request(inspect);
  test.expect(request && std::holds_alternative<cloth::ShuttleV2InspectRequest>(
                             *request),
              "valid protocol-v2 inspect request was rejected");

  const std::vector<std::filesystem::path> link{"--shuttle-protocol",
                                                "2",
                                                "--operation",
                                                "link",
                                                "--target",
                                                "x86_64",
                                                "--output",
                                                temporary.path() / "app.exe",
                                                "--root-package",
                                                "app",
                                                "--entry",
                                                "Main.co",
                                                "--artifact",
                                                "app",
                                                "1.2.3",
                                                digest,
                                                artifact_path};
  request = cloth::prepare_shuttle_v2_request(link);
  test.expect(
      request && std::holds_alternative<cloth::ShuttleV2LinkRequest>(*request),
      "valid protocol-v2 link request was rejected");

  auto invalid = compile;
  invalid.insert(invalid.end(), {"--input", artifact_path});
  test.expect(!cloth::prepare_shuttle_v2_request(invalid),
              "compile accepted an inspect-only option");
  invalid = compile;
  invalid[1] = "1";
  test.expect(!cloth::prepare_shuttle_v2_request(invalid),
              "protocol-v2 parser accepted protocol 1");
  invalid = compile;
  invalid.insert(invalid.end(), {"--artifact-kind", "interface"});
  test.expect(!cloth::prepare_shuttle_v2_request(invalid),
              "duplicate singleton option was accepted");
  invalid = link;
  invalid.insert(invalid.end(),
                 {"--artifact", "app", "1.2.3", digest, artifact_path});
  test.expect(!cloth::prepare_shuttle_v2_request(invalid),
              "duplicate package artifact was accepted");
}

void protocol_v2_json_contract(TestContext& test) {
  const cloth::ArtifactDigest digest = cloth::sha256("compiler");
  const std::string capabilities = cloth::shuttle_capabilities_json(digest);
  test.expect(capabilities.starts_with("{\"schema\":1,\"protocols\":[1,2]") &&
                  capabilities.contains("\"operations\":[\"compile\","
                                        "\"inspect\",\"link\"]") &&
                  capabilities.contains(cloth::artifact_digest_hex(digest)) &&
                  !capabilities.ends_with('\n'),
              "capability response does not match protocol schema 1");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"valid protocol plan", valid_protocol_plan},
      {"package dependency import", package_dependency_import},
      {"invalid protocol graphs", invalid_protocol_graphs},
      {"protocol configuration validation", protocol_configuration_validation},
      {"output configuration validation", output_configuration_validation},
      {"protocol v2 operations", protocol_v2_operations},
      {"protocol v2 JSON contract", protocol_v2_json_contract},
  };
  return cloth::test::run_tests(tests);
}
