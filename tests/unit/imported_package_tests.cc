// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/compiler/compilation.h"
#include "cloth/identity/canonical_identity.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

struct PackageGraph {
  explicit PackageGraph(std::string prefix = "checkout", bool reverse = false) {
    compilation.set_package_dependencies({{"app", "dep", "models"}});
    const auto add_models = [&] {
      compilation.add_package_source(
          cloth::SourceFile::from_memory(
              prefix + "/models/Base.co",
              "int32 Count;\n"
              "static final int64 Version = 12;\n"
              "Base(int32 count) { Count = count; }\n"
              "base() { Count = 0; }\n"
              "func Read(): int32 { return Count; }\n"
              "func hidden(): int32 { return Count; }\n"
              "func Maybe(Base? value): Base? { return value; }\n"
              "func Many(Base?[] values): Base?[] { return values; }\n"),
          "models", "", "1.2.3");
      compilation.add_package_source(
          cloth::SourceFile::from_memory(prefix + "/models/geometry/Shape.co",
                                         "interface { func Size(): int32; }\n"),
          "models", "geometry", "1.2.3");
    };
    const auto add_app = [&] {
      compilation.add_package_source(
          cloth::SourceFile::from_memory(
              prefix + "/app/Derived.co",
              "import dep::Base;\n"
              "import dep.geometry::Shape;\n"
              "class : Base is Shape {\n"
              "  Derived(int32 count): Base(count) {}\n"
              "  override func Read(): int32 { return super.Read(); }\n"
              "  func Size(): int32 { return Read(); }\n"
              "}\n"),
          "app", "", "0.4.0");
    };
    if (reverse) {
      add_app();
      add_models();
    } else {
      add_models();
      add_app();
    }
    result.emplace(compilation.analyze(diagnostics));
  }

  [[nodiscard]] std::string diagnostic_text() const {
    std::string result;
    for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
      if (!result.empty()) result += "; ";
      result += diagnostic.message;
    }
    return result;
  }

  cloth::Compilation compilation;
  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;
};

const cloth::ImportedFile* find_file(const cloth::ImportedPackageView& view,
                                     std::string_view name) {
  const auto file = std::ranges::find_if(
      view.files, [&](const cloth::ImportedFile& candidate) {
        return candidate.nominal_identity.name == name;
      });
  return file == view.files.end() ? nullptr : &*file;
}

const cloth::ImportedMember* find_member(const cloth::ImportedFile& file,
                                         std::string_view name) {
  const auto member =
      std::ranges::find(file.members, name, &cloth::ImportedMember::name);
  return member == file.members.end() ? nullptr : &*member;
}

cloth::ImportedPackageResult build_models(const PackageGraph& graph) {
  return cloth::build_imported_package_view(
      {"models", "1.2.3"}, graph.result->semantics, graph.result->mir,
      graph.result->abi);
}

std::string issue_text(const cloth::ImportedPackageResult& result) {
  std::string text;
  for (const cloth::ImportedPackageIssue& issue : result.issues) {
    if (!text.empty()) text += "; ";
    text += issue.record + ": " + issue.message;
  }
  return text;
}

void owns_declarations_and_static_values(TestContext& test) {
  const PackageGraph graph;
  test.expect(graph.result->is_valid,
              "package graph failed compilation: " + graph.diagnostic_text());
  if (!graph.result->is_valid) return;
  auto imported = build_models(graph);
  test.expect(imported.is_valid(),
              "valid package view was rejected: " + issue_text(imported));
  if (!imported.view) return;

  test.expect(imported.view->files.size() == 2 &&
                  find_file(*imported.view, "Derived") == nullptr,
              "package view retained a consumer-owned declaration");
  test.expect(std::ranges::none_of(imported.view->types,
                                   [](const cloth::ImportedType& type) {
                                     return type.nominal_identity &&
                                            type.nominal_identity->name ==
                                                "Derived";
                                   }),
              "type table leaked an unrelated consumer type");
  const cloth::ImportedFile* base = find_file(*imported.view, "Base");
  test.expect(base != nullptr && base->logical_path == "Base.co",
              "owned file or logical source path is missing");
  if (base == nullptr) return;
  const cloth::ImportedMember* private_function = find_member(*base, "hidden");
  const cloth::ImportedMember* private_constructor = find_member(*base, "base");
  const cloth::ImportedMember* version = find_member(*base, "Version");
  test.expect(
      private_function != nullptr && private_constructor != nullptr &&
          private_function->visibility == cloth::Visibility::kPrivate &&
          private_constructor->visibility == cloth::Visibility::kPrivate,
      "private declarations were discarded or promoted");
  test.expect(version != nullptr && version->static_value &&
                  version->static_value->kind == cloth::LiteralKind::kInteger &&
                  version->static_value->lexeme == "12",
              "typed static literal was not retained");
  test.expect(base->member_order.size() == base->members.size(),
              "source declaration order was not retained explicitly");
  test.expect(base->location.path == "Base.co" && base->location.line == 1 &&
                  base->location.column == 1,
              "file declaration location was not detached");
}

void retains_inheritance_interfaces_and_abi(TestContext& test) {
  const PackageGraph graph;
  test.expect(graph.result->is_valid,
              "package graph failed compilation: " + graph.diagnostic_text());
  if (!graph.result->is_valid) return;
  auto imported = cloth::build_imported_package_view(
      {"app", "0.4.0"}, graph.result->semantics, graph.result->mir,
      graph.result->abi);
  test.expect(imported.is_valid(),
              "consumer package view was rejected: " + issue_text(imported));
  if (!imported.view) return;
  const cloth::ImportedFile* derived = find_file(*imported.view, "Derived");
  test.expect(derived != nullptr && derived->base_identity &&
                  derived->direct_interface_identities.size() == 1 &&
                  derived->interface_implementations.size() == 1,
              "inheritance or interface conformance metadata was lost");
  if (derived == nullptr) return;
  test.expect(
      derived->abi.fields.size() == 1 &&
          derived->abi.descriptor.parent_identity == derived->base_identity &&
          derived->abi.descriptor.interfaces.size() == 1 &&
          derived->abi.descriptor.reference_offsets.empty(),
      "flattened layout or descriptor dispatch metadata was lost");
  const cloth::ImportedMember* constructor = find_member(*derived, "Derived");
  const auto callable = std::ranges::find_if(
      derived->abi.callables, [&](const cloth::ImportedCallableAbi& candidate) {
        return constructor != nullptr &&
               candidate.member_identity == constructor->identity;
      });
  test.expect(
      constructor != nullptr && constructor->base_constructor_identity &&
          callable != derived->abi.callables.end() &&
          callable->initializer_identity &&
          callable->initializer_linkage == cloth::AbiLinkage::kExternal &&
          callable->initializer_parameters.size() == 2 &&
          callable->initializer_parameters.front().kind ==
              cloth::AbiParameterKind::kReceiver &&
          callable->initializer_parameters.front().type_identity ==
              derived->identity,
      "constructor selection or initializer ABI was lost");

  auto broken_dispatch = *imported.view;
  ++broken_dispatch.files[0].abi.descriptor.interfaces[0].interface_id;
  test.expect(!cloth::verify_imported_package_view(broken_dispatch).empty(),
              "corrupt interface dispatch ID was accepted");
  auto broken_parent = *imported.view;
  broken_parent.files[0].abi.descriptor.parent_identity.reset();
  test.expect(!cloth::verify_imported_package_view(broken_parent).empty(),
              "corrupt descriptor ancestry was accepted");
}

void remains_owned_after_compilation_destruction(TestContext& test) {
  cloth::ImportedPackageView detached;
  {
    const PackageGraph graph;
    test.expect(graph.result->is_valid,
                "package graph failed compilation: " + graph.diagnostic_text());
    if (!graph.result->is_valid) return;
    auto imported = build_models(graph);
    test.expect(imported.is_valid(),
                "valid package view was rejected: " + issue_text(imported));
    if (!imported.view) return;
    detached = std::move(*imported.view);
  }
  const cloth::ImportedFile* base = find_file(detached, "Base");
  const cloth::ImportedMember* read =
      base == nullptr ? nullptr : find_member(*base, "Read");
  test.expect(base != nullptr &&
                  base->nominal_identity.package.version == "1.2.3" &&
                  read != nullptr && read->location.path == "Base.co" &&
                  read->location.line == 5,
              "imported view retained source-backed or compilation-owned data");
}

void deterministic_and_corruption_checked(TestContext& test) {
  const PackageGraph first;
  const PackageGraph second{"relocated tree", true};
  test.expect(first.result->is_valid && second.result->is_valid,
              "determinism graphs failed compilation: " +
                  first.diagnostic_text() + "; " + second.diagnostic_text());
  if (!first.result->is_valid || !second.result->is_valid) return;
  auto left = build_models(first);
  auto right = build_models(second);
  test.expect(left.is_valid() && right.is_valid() && left.view == right.view,
              "view depends on source registration order or source path: " +
                  issue_text(left) + "; " + issue_text(right));
  if (!left.view) return;

  auto expect_rejected = [&](cloth::ImportedPackageView broken,
                             std::string_view message) {
    test.expect(!cloth::verify_imported_package_view(broken).empty(), message);
  };
  auto broken = *left.view;
  broken.files[0].abi.descriptor.mangled_name.clear();
  expect_rejected(std::move(broken), "corrupt descriptor name was accepted");
  broken = *left.view;
  broken.files[0].members[0].identity.push_back('x');
  expect_rejected(std::move(broken),
                  "corrupt declaration identity was accepted");
  broken = *left.view;
  broken.files[0].member_order.push_back(broken.files[0].member_order.front());
  expect_rejected(std::move(broken),
                  "duplicate declaration order was accepted");
  broken = *left.view;
  broken.types[0].storage.alignment = 3;
  expect_rejected(std::move(broken), "corrupt target type layout was accepted");
  broken = *left.view;
  std::ranges::reverse(broken.files);
  expect_rejected(std::move(broken), "noncanonical file order was accepted");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"owns declarations and static values",
       owns_declarations_and_static_values},
      {"retains inheritance, interfaces, and ABI",
       retains_inheritance_interfaces_and_abi},
      {"remains owned after compilation destruction",
       remains_owned_after_compilation_destruction},
      {"deterministic and corruption checked",
       deterministic_and_corruption_checked},
  };
  return cloth::test::run_tests(tests);
}
