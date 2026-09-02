// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_verifier.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/identity/canonical_identity.h"
#include "cloth/identity/package_identity.h"
#include "cloth/sema/canonical_identity.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

struct Graph {
  explicit Graph(std::string prefix = "original", std::string alias = "models",
                 std::string version = "1.2.3+local", bool reverse = false)
      : package{"models", std::move(version)} {
    compilation.set_package_dependencies({{"app", alias, "models"}});
    const auto add_base = [&] {
      compilation.add_package_source(
          cloth::SourceFile::from_memory(
              prefix + "/Base.co",
              "int32 Value;\nstatic final int32 Version = 1;\n"
              "Base(int32 value) { Value = value; }\n"
              "base() { Value = 0; }\n"
              "func Read(): int32 { return Value; }\nfunc hidden() {}\n"),
          package.name, "", package.version);
      compilation.add_package_source(
          cloth::SourceFile::from_memory(prefix + "/Shape.co",
                                         "interface { func Size(): int32; }\n"),
          package.name, "geometry", package.version);
    };
    const auto add_derived = [&] {
      compilation.add_package_source(
          cloth::SourceFile::from_memory(
              prefix + "/Derived.co",
              "import " + alias +
                  "::Base;\nclass : Base {\n"
                  "Derived(int32 value): Base(value) {}\n"
                  "override func Read(): int32 { return super.Read() + "
                  "Base.Version; }\n}\n"),
          "app", "", "0.1.0");
    };
    if (reverse) {
      add_derived();
      add_base();
    } else {
      add_base();
      add_derived();
    }
    result.emplace(compilation.analyze(diagnostics));
  }

  const cloth::AbiFileClass* file(std::string_view name) const {
    for (const auto& candidate : result->abi.files) {
      if (result->semantics.symbol(candidate.symbol).name == name) {
        return &candidate;
      }
    }
    return nullptr;
  }

  std::map<std::string, std::string> names() const {
    std::map<std::string, std::string> values;
    for (const auto& file : result->abi.files) {
      const std::string& owner = result->semantics.symbol(file.symbol).name;
      values.emplace(owner, file.type_descriptor.mangled_name);
      for (const auto& function : file.functions) {
        values.emplace(
            owner + "." + result->semantics.symbol(function.symbol).name,
            function.mangled_name);
      }
      for (const auto& constructor : file.constructors) {
        const std::string key =
            owner + "." + result->semantics.symbol(constructor.symbol).name;
        values.emplace(key, constructor.mangled_name);
        values.emplace(key + ".initializer",
                       constructor.initializer_mangled_name);
      }
      for (const auto& field : file.static_fields) {
        values.emplace(
            owner + "." + result->semantics.symbol(field.symbol).name,
            field.mangled_name);
      }
    }
    return values;
  }

  cloth::PackageIdentity package;
  cloth::Compilation compilation;
  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;
};

void fixed_encoding_vectors(TestContext& test) {
  test.expect(
      cloth::mangle_canonical_identity(
          cloth::canonical_primitive_identity("int32")) ==
          "_C309000000000000007072696d69746976650500000000000000696e743332",
      "primitive identity does not match the ABI-2 byte fixture");
  test.expect(
      cloth::mangle_canonical_identity(
          cloth::canonical_nominal_identity({{}, "", "A"})) ==
          "_C3"
          "07000000000000006e6f6d696e616c0a000000000000007374616e64616c6f6e65"
          "000000000000000000000000000000000000000000000000010000000000000041"
          "0500000000000000636c617373",
      "standalone nominal identity does not match its byte fixture");
  test.expect(cloth::mangle_canonical_identity(std::string_view{"\0\xff", 2}) ==
                  "_C300ff",
              "mangling depends on signed char or terminates at NUL");
  const cloth::NominalIdentity interface{{"models", "1.2.3+local"},
                                         "geometry",
                                         "Shape",
                                         cloth::NominalKind::kInterface};
  test.expect(
      cloth::canonical_interface_id(interface) == 15932219155100572591ULL,
      "interface ID does not match the canonical FNV-1a fixture");
}

void separated_identity_domains(TestContext& test) {
  const cloth::NominalIdentity standalone{{}, "models", "User"};
  const cloth::NominalIdentity package{{"models", "0.1.0"}, "", "User"};
  test.expect(cloth::canonical_nominal_identity(standalone) !=
                  cloth::canonical_nominal_identity(package),
              "standalone namespace aliases a manifest package identity");
  test.expect(cloth::canonical_nominal_identity({{}, "ab.c", "User"}) !=
                  cloth::canonical_nominal_identity({{}, "a.bc", "User"}),
              "namespace components are not length delimited");
  auto interface = package;
  interface.kind = cloth::NominalKind::kInterface;
  test.expect(cloth::canonical_nominal_identity(package) !=
                  cloth::canonical_nominal_identity(interface),
              "class and interface identities share a domain");
  const auto type = cloth::canonical_nominal_identity(package);
  test.expect(type != cloth::canonical_array_identity(type) &&
                  cloth::canonical_array_identity(type) !=
                      cloth::canonical_nullable_identity(type),
              "structural type constructors share a domain");
  const std::array kinds{cloth::CanonicalMemberKind::kFunction,
                         cloth::CanonicalMemberKind::kConstructor,
                         cloth::CanonicalMemberKind::kConstructorInitializer,
                         cloth::CanonicalMemberKind::kStaticField,
                         cloth::CanonicalMemberKind::kInstanceField,
                         cloth::CanonicalMemberKind::kDescriptor};
  std::vector<std::string> identities;
  for (const auto kind : kinds) {
    identities.push_back(
        cloth::canonical_member_identity(package, kind, "User"));
  }
  std::ranges::sort(identities);
  test.expect(std::adjacent_find(identities.begin(), identities.end()) ==
                  identities.end(),
              "different member kinds have the same identity");
  const std::vector<std::string> one{"ab", "c"};
  const std::vector<std::string> two{"a", "bc"};
  test.expect(
      cloth::canonical_member_identity(package, kinds[0], "Call", one) !=
          cloth::canonical_member_identity(package, kinds[0], "Call", two),
      "parameter identities are not length delimited");
}

void relocation_alias_and_order_independence(TestContext& test) {
  const Graph first;
  const Graph second{"relocated path", "renamed", "1.2.3+local", true};
  test.expect(first.result->is_valid && second.result->is_valid,
              "identity comparison graph failed compilation");
  if (!first.result->is_valid || !second.result->is_valid) return;
  test.expect(first.names() == second.names(),
              "canonical names depend on aliases, paths, or insertion order");
  const auto* left = first.file("models.geometry.Shape");
  const auto* right = second.file("models.geometry.Shape");
  test.expect(left && right && left->file != right->file,
              "comparison did not perturb local file handles");
  if (left && right) {
    test.expect(first.result->semantics.file(left->file).interface_id ==
                    second.result->semantics.file(right->file).interface_id,
                "interface identity depends on local file handles");
  }
}

void exact_version_identity(TestContext& test) {
  const Graph first;
  const Graph second{"other", "models", "1.2.3+different"};
  test.expect(first.result->is_valid && second.result->is_valid,
              "version comparison graph failed compilation");
  if (!first.result->is_valid || !second.result->is_valid) return;
  const auto* left = first.file("models.Base");
  const auto* right = second.file("models.Base");
  test.expect(left && right, "version change affected the source-visible name");
  if (!left || !right) return;
  test.expect(left->type_descriptor.name == right->type_descriptor.name &&
                  left->type_descriptor.name == "models.Base",
              "ABI version leaked into source-visible type names");
  test.expect(
      left->type_descriptor.mangled_name !=
              right->type_descriptor.mangled_name &&
          left->functions[0].mangled_name != right->functions[0].mangled_name &&
          left->constructors[0].initializer_mangled_name !=
              right->constructors[0].initializer_mangled_name &&
          left->static_fields[0].mangled_name !=
              right->static_fields[0].mangled_name,
      "exact package version was omitted from an external symbol");
  auto identity =
      first.result->semantics.file(first.file("models.geometry.Shape")->file)
          .identity;
  const auto initial = cloth::canonical_interface_id(identity);
  identity.package.version = "1.2.3+different";
  test.expect(initial != cloth::canonical_interface_id(identity),
              "interface identity discarded SemVer build metadata");
}

void semantic_and_overload_type_identity(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_source(cloth::SourceFile::from_memory(
      "Value.co",
      "func Echo(Value? value): Value? { return value; }\n"
      "func Int(int value): int32 { return value; }\n"
      "func Float(float value): float32 { return value; }\n"));
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, "type identity fixture failed compilation");
  if (!result.is_valid) return;
  const auto& file = result.semantics.file({0});
  const auto nullable =
      result.semantics.symbol(file.functions[0]).parameter_types[0];
  test.expect(cloth::canonical_type_identity(nullable, result.semantics) !=
                  cloth::canonical_type_identity(file.type, result.semantics),
              "semantic type identity erased nullability");
  test.expect(
      cloth::canonical_type_identity(nullable, result.semantics,
                                     cloth::TypeIdentityMode::kOverload) ==
          cloth::canonical_type_identity(file.type, result.semantics,
                                         cloth::TypeIdentityMode::kOverload),
      "overload identity changed nullability erasure");
  test.expect(
      cloth::canonical_type_identity(
          result.semantics.symbol(file.functions[1]).parameter_types[0],
          result.semantics) == cloth::canonical_primitive_identity("int32") &&
          cloth::canonical_type_identity(
              result.semantics.symbol(file.functions[2]).parameter_types[0],
              result.semantics) ==
              cloth::canonical_primitive_identity("float32"),
      "primitive aliases acquired distinct identities");
}

void ownership_partition(TestContext& test) {
  Graph graph;
  test.expect(graph.result->is_valid, "partition fixture failed compilation");
  if (!graph.result->is_valid) return;
  const auto& result = *graph.result;
  const auto* base = graph.file("models.Base");
  const auto* derived = graph.file("app.Derived");
  if (!base || !derived) {
    test.expect(false, "partition types are missing");
    return;
  }
  cloth::LlvmIrOptions options;
  options.package = graph.package;
  const auto dependency = cloth::emit_llvm_ir(
      result.mir, result.abi, result.semantics, graph.diagnostics, options);
  options.package = cloth::PackageIdentity{"app", "0.1.0"};
  const auto consumer = cloth::emit_llvm_ir(
      result.mir, result.abi, result.semantics, graph.diagnostics, options);
  test.expect(dependency && consumer && !graph.diagnostics.has_errors(),
              "package IR partition failed");
  if (!dependency || !consumer) return;
  test.expect(
      dependency->text.contains("@" + base->type_descriptor.mangled_name +
                                " = constant") &&
          consumer->text.contains("@" + base->type_descriptor.mangled_name +
                                  " = external constant") &&
          consumer->text.contains("@" + derived->type_descriptor.mangled_name +
                                  " = constant"),
      "descriptor ownership was duplicated or lost");
  const auto& initializer = base->constructors[0].initializer_mangled_name;
  test.expect(dependency->text.contains("define void @" + initializer +
                                        "(ptr %self, i32 ") &&
                  consumer->text.contains("declare void @" + initializer +
                                          "(ptr, i32)") &&
                  consumer->text.contains("call void @" + initializer +
                                          "(ptr %self, i32 "),
              "base initialization does not cross the package ABI boundary");
  test.expect(
      consumer->text.contains("declare i32 @" +
                              base->functions[0].mangled_name + "(ptr)") &&
          consumer->text.contains("call i32 @" +
                                  base->functions[0].mangled_name) &&
          consumer->text.contains("@" + base->static_fields[0].mangled_name +
                                  " = external constant i32"),
      "imported call or static storage has no ABI declaration");
  test.expect(
      !consumer->text.contains(base->functions[1].mangled_name) &&
          !consumer->text.contains(base->constructors[1].mangled_name) &&
          !consumer->text.contains(
              base->constructors[1].initializer_mangled_name),
      "private dependency implementation leaked into the consumer");
  test.expect(!dependency->text.contains("define i32 @main(") &&
                  !consumer->text.contains("define i32 @main("),
              "package output emitted a native entry wrapper");
  options.emit_native_entry_point = true;
  test.expect(!cloth::emit_llvm_ir(result.mir, result.abi, result.semantics,
                                   graph.diagnostics, options),
              "package module accepted native entry generation");
  options.emit_native_entry_point = false;
  options.package = cloth::PackageIdentity{"missing", "0.1.0"};
  test.expect(!cloth::emit_llvm_ir(result.mir, result.abi, result.semantics,
                                   graph.diagnostics, options),
              "unknown package silently emitted an empty module");
}

void rejects_identity_and_linkage_corruption(TestContext& test) {
  const Graph graph;
  test.expect(graph.result->is_valid,
              "ABI corruption fixture failed compilation");
  if (!graph.result->is_valid) return;
  for (int mutation = 0; mutation < 3; ++mutation) {
    auto broken = graph.result->abi;
    if (mutation == 0) broken.files[0].type_descriptor.mangled_name.clear();
    if (mutation == 1)
      broken.files[0].constructors[0].initializer_linkage =
          cloth::AbiLinkage::kInternal;
    if (mutation == 2)
      broken.files[0].constructors[1].initializer_linkage =
          cloth::AbiLinkage::kExternal;
    cloth::DiagnosticEngine diagnostics;
    test.expect(
        !cloth::verify_abi(broken, graph.result->mir, graph.result->semantics,
                           diagnostics),
        "ABI verifier accepted corrupted identity or initializer linkage");
  }
}

void rejects_invalid_package_identity(TestContext& test) {
  for (const std::string_view invalid :
       {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.2.3-01", "1.2.3+"}) {
    test.expect(!cloth::is_valid_package_version(invalid),
                "invalid exact package version was accepted");
  }
  test.expect(cloth::is_valid_package_version("1.2.3-rc.1+local.001") &&
                  cloth::is_valid_package_name("data-models"),
              "valid package identity was rejected");
  for (const std::string_view invalid :
       {"", "Models", "-models", "models-", "data--models", "models.name"}) {
    test.expect(!cloth::is_valid_package_name(invalid),
                "invalid owning package name was accepted");
  }
  cloth::Compilation compilation;
  compilation.add_package_source(cloth::SourceFile::from_memory("A.co", ""),
                                 "models", "", "1.0.0");
  compilation.add_package_source(cloth::SourceFile::from_memory("B.co", ""),
                                 "models", "", "2.0.0");
  cloth::DiagnosticEngine diagnostics;
  test.expect(!compilation.analyze(diagnostics).is_valid,
              "multiple versions of one owning package were accepted");
  cloth::Compilation missing;
  missing.add_package_source(cloth::SourceFile::from_memory("A.co", ""),
                             "models", "", "");
  cloth::DiagnosticEngine missing_diagnostics;
  test.expect(!missing.analyze(missing_diagnostics).is_valid,
              "package source silently lost its version");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"fixed encoding vectors", fixed_encoding_vectors},
      {"separated identity domains", separated_identity_domains},
      {"relocation, alias, and order independence",
       relocation_alias_and_order_independence},
      {"exact version identity", exact_version_identity},
      {"semantic and overload type identity",
       semantic_and_overload_type_identity},
      {"ownership partition", ownership_partition},
      {"rejects identity and linkage corruption",
       rejects_identity_and_linkage_corruption},
      {"rejects invalid package identity", rejects_invalid_package_identity},
  };
  return cloth::test::run_tests(tests);
}
