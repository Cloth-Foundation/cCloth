// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/artifact/imported_package.h"
#include "cloth/compiler/compilation.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test.h"

namespace {
using cloth::test::TestCase;
using cloth::test::TestContext;

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string result;
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    result += diagnostic.message + "\n";
  }
  return result;
}

void add(cloth::Compilation& compilation, std::string name, std::string text) {
  compilation.add_source(
      cloth::SourceFile::from_memory(std::move(name), std::move(text)));
}

void explicit_contracts(TestContext& test) {
  cloth::Compilation compilation;
  // Descendants precede parents to exercise order-independent resolution.
  add(compilation, "Derived.co", R"(
    class : Abstract {
      override func Read(): string { return "derived"; }
    }
  )");
  add(compilation, "Abstract.co", R"(
    abstract class is Refined {
      abstract override func Read(): string;
    }
  )");
  add(compilation, "Refined.co",
      "interface : Readable { func Read(): string; }");
  add(compilation, "Readable.co", "interface { func Read(): object; }");
  add(compilation, "Other.co", "interface { func Read(): string; }");
  add(compilation, "Overloaded.co",
      "interface { func Read(int32 n): string; }");
  add(compilation, "Concrete.co", R"(
    class is Refined, Other, Overloaded {
      final override func Read(): string { return "value"; }
      override func Read(int32 n): string { return Read(); }
      func Read(bool flag): string { return Read(); }
    }
  )");
  add(compilation, "Inherited.co", "class : Concrete {}");
  add(compilation, "Plain.co", "func Read(): string { return \"plain\"; }");
  add(compilation, "Adopted.co", "class : Plain is Refined {}");
  add(compilation, "Both.co", R"(
    class : Plain is Refined, Other {
      override func Read(): string { return super.Read(); }
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto& semantics = result.semantics;
  const auto& abstract =
      semantics.symbol(semantics.file({1}).functions.front());
  const auto& concrete =
      semantics.symbol(semantics.file({6}).functions.front());
  const auto& derived = semantics.symbol(semantics.file({0}).functions.front());
  const auto& both = semantics.symbol(semantics.file({10}).functions.front());
  test.expect(abstract.is_override && abstract.is_abstract &&
                  abstract.virtual_slot && !abstract.overridden_symbol,
              "abstract interface implementation must introduce a class slot");
  test.expect(
      concrete.is_override && concrete.is_final && concrete.virtual_slot &&
          !concrete.overridden_symbol,
      "interface-only override must not invent a replaced class member");
  test.expect(
      derived.overridden_symbol == semantics.file({1}).functions.front() &&
          derived.virtual_slot == abstract.virtual_slot &&
          both.overridden_symbol == semantics.file({8}).functions.front(),
      "base replacement identity or slot was lost");
  test.expect(semantics.file({7}).interface_implementations.size() == 4 &&
                  semantics.file({9}).interface_implementations.size() == 2,
              "inherited implementations must satisfy interfaces without "
              "redeclaration");
}

void covariant_interface_return_order(TestContext& test) {
  const std::vector<std::pair<std::string, std::string>> sources{
      {"Contract.co", "interface { func Read(); }"},
      {"Base.co", "func Make(): Contract { return Product(); }"},
      {"Derived.co",
       "class : Base { override func Make(): Product { return Product(); } }"},
      {"Product.co",
       "class is Contract { Product() {} override func Read() {} }"},
  };
  for (const bool reverse : {false, true}) {
    cloth::Compilation compilation;
    for (std::size_t i = 0; i < sources.size(); ++i) {
      const auto& [name, body] = sources[reverse ? sources.size() - 1 - i : i];
      add(compilation, name, body);
    }
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze(diagnostics);
    test.expect(result.is_valid,
                "interface covariance depends on source order\n" +
                    messages(diagnostics));
  }
}

void invalid_contracts(TestContext& test) {
  const std::vector<std::pair<std::string, std::string>> cases{
      {"class is Contract { func Read(): int32 { return 1; } }",
       "implements an interface function; add 'override'"},
      {"class is Child { func Read(): int32 { return 1; } }",
       "implements an interface function; add 'override'"},
      {"abstract class is Contract { abstract func Read(): int32; }",
       "implements an interface function; add 'override'"},
      {"class : Obligated { func Read(): int32 { return 1; } }",
       "implements an interface function; add 'override'"},
      {"override func Read(): int32 { return 1; }",
       "does not override an inherited class or interface function"},
      {"class is Contract { override func Read(int32 n): int32 { return n; } }",
       "does not override an inherited class or interface function"},
      {"class is Contract { override func Write(): int32 { return 1; } }",
       "does not override an inherited class or interface function"},
      {"class is Contract { override func read(): int32 { return 1; } }",
       "private function 'read' cannot be declared override"},
      {"class is Contract { static override func Read(): int32 { return 1; } }",
       "static function 'Read' cannot be declared override"},
      {"class is Contract { override func Read(): int64 { return 1; } }",
       "implementation of interface function 'Read' returns"},
      {"class : Closed { override func Read(): int32 { return 1; } }",
       "cannot override inherited final function"},
      {"class is Contract { override func Read(): int32 { return super.Read(); "
       "} }",
       "'super'"},
      {"interface : Contract { override func Read(): int32; }",
       "interface function contracts do not accept function modifiers"},
      {"struct { override func Read(): int32 { return 1; } }",
       "struct functions cannot be abstract, override, or final"},
  };
  for (const auto& [source, expected] : cases) {
    cloth::Compilation compilation;
    add(compilation, "Contract.co", "interface { func Read(): int32; }");
    add(compilation, "Child.co", "interface : Contract {}");
    add(compilation, "Obligated.co", "abstract class is Contract {}");
    add(compilation, "Closed.co",
        "class is Contract { final override func Read(): int32 { return 1; } "
        "}");
    add(compilation, "Subject.co", source);
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze_frontend(diagnostics);
    test.expect(
        !result.is_valid && messages(diagnostics).contains(expected),
        source + "\nExpected: " + expected + "\n" + messages(diagnostics));
  }
}

cloth::ImportedMember& member(cloth::ImportedPackageView& view,
                              std::string_view owner, std::string_view name) {
  auto& file = *std::ranges::find_if(view.files, [&](const auto& item) {
    return item.nominal_identity.name == owner;
  });
  return *std::ranges::find(file.members, name, &cloth::ImportedMember::name);
}

bool has_issue(const std::vector<cloth::ImportedPackageIssue>& issues,
               std::string_view text) {
  return std::ranges::any_of(
      issues, [&](const auto& issue) { return issue.message.contains(text); });
}

void imported_contracts(TestContext& test) {
  cloth::Compilation compilation;
  compilation.set_package_dependencies({{"app", "dep", "contracts"}});
  compilation.add_package_source(
      cloth::SourceFile::from_memory("Contract.co",
                                     "interface { func Read(): int32; }"),
      "contracts", "", "1.0.0");
  compilation.add_package_source(
      cloth::SourceFile::from_memory("Plain.co",
                                     "func Read(): int32 { return 1; }"),
      "contracts", "", "1.0.0");
  const auto add_app = [&](std::string name, std::string body) {
    compilation.add_package_source(
        cloth::SourceFile::from_memory(
            std::move(name),
            "import dep::Contract; import dep::Plain; " + body),
        "app", "", "1.0.0");
  };
  add_app("Local.co", "interface { func LocalRead(): int32; }");
  add_app("Concrete.co", R"(
    class is Contract, Local {
      override func Read(): int32 { return 1; }
      override func LocalRead(): int32 { return 2; }
      func Ordinary(): int32 { return 3; }
    }
  )");
  add_app("Adopted.co", "class : Plain is Contract {}");
  add_app("Both.co", R"(
    class : Plain is Contract {
      override func Read(): int32 { return super.Read(); }
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  auto dependency = cloth::build_imported_package_view(
      {"contracts", "1.0.0"}, result.semantics, result.mir, result.abi);
  auto app = cloth::build_imported_package_view(
      {"app", "1.0.0"}, result.semantics, result.mir, result.abi);
  test.expect(dependency.is_valid() && app.is_valid(),
              "valid override export failed");
  if (!dependency.is_valid() || !app.is_valid()) return;
  const auto closure_issues = [&](const cloth::ImportedPackageView& candidate) {
    const cloth::ImportedPackageView* packages[]{&*dependency.view, &candidate};
    return cloth::verify_imported_package_closure(packages);
  };
  test.expect(closure_issues(*app.view).empty(),
              "valid override closure failed");
  auto broken = *app.view;
  member(broken, "Concrete", "Read").is_override = false;
  test.expect(
      has_issue(closure_issues(broken), "implementation requires override"),
      "foreign interface implementation lost its required marker");
  broken = *app.view;
  member(broken, "Concrete", "LocalRead").is_override = false;
  test.expect(has_issue(cloth::verify_imported_package_view(broken),
                        "implementation requires override"),
              "local interface implementation lost its required marker");
  broken = *app.view;
  member(broken, "Concrete", "Ordinary").is_override = true;
  test.expect(has_issue(closure_issues(broken),
                        "override has no class or interface contract"),
              "spurious imported marker was accepted");
  broken = *app.view;
  member(broken, "Concrete", "Read").overridden_identity =
      member(*dependency.view, "Contract", "Read").identity;
  test.expect(
      has_issue(closure_issues(broken), "does not match override target"),
      "interface contract was accepted as a replaced class member");
  broken = *app.view;
  member(broken, "Both", "Read").overridden_identity.reset();
  test.expect(
      has_issue(closure_issues(broken), "does not match override target"),
      "base override lost its replaced member identity");
  broken = *app.view;
  member(broken, "Concrete", "Read").is_static = true;
  test.expect(has_issue(closure_issues(broken),
                        "override requires a public class instance function"),
              "static imported override was accepted");
  for (const bool marked : {false, true}) {
    cloth::Compilation consumer;
    consumer.set_package_dependencies({{"consumer", "dep", "contracts"}});
    consumer.add_imported_package(*dependency.view);
    consumer.add_package_source(
        cloth::SourceFile::from_memory(
            "Consumer.co", "import dep::Contract; class is Contract { " +
                               std::string{marked ? "override " : ""} +
                               "func Read(): int32 { return 1; } }"),
        "consumer", "", "1.0.0");
    cloth::DiagnosticEngine imported_diagnostics;
    const auto imported_result = consumer.analyze(imported_diagnostics);
    test.expect(imported_result.is_valid == marked,
                "source-free consumer did not enforce explicit override\n" +
                    messages(imported_diagnostics));
  }
}
}  // namespace

int main() {
  const TestCase tests[]{
      {"explicit contracts", explicit_contracts},
      {"covariant interface return order", covariant_interface_return_order},
      {"invalid contracts", invalid_contracts},
      {"imported contracts", imported_contracts},
  };
  return cloth::test::run_tests(tests);
}
