#include "cloth/compiler/compilation.h"
#include "cloth/compiler/shuttle_protocol.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
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
    compilation.add_package_source(std::move(source.source),
                                   std::move(source.package),
                                   std::move(source.source_package));
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

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"valid protocol plan", valid_protocol_plan},
      {"package dependency import", package_dependency_import},
      {"invalid protocol graphs", invalid_protocol_graphs},
  };
  return cloth::test::run_tests(tests);
}
