// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/compilation.h"

#include "cloth/abi/abi.h"
#include "cloth/abi/abi_verifier.h"
#include "cloth/flow/control_flow.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/identity/package_identity.h"
#include "cloth/lexer/lexer.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_optimizer.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/parser/parser.h"
#include "cloth/parser/syntax_facts.h"
#include "cloth/sema/semantic_analyzer.h"
#include "cloth/source/path.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cloth {
namespace {

std::string qualified_name(std::string_view package_name,
                           std::string_view class_name) {
  if (package_name.empty()) {
    return std::string{class_name};
  }
  return std::string{package_name} + '.' + std::string{class_name};
}

std::string qualified_name(std::string_view owning_package,
                           std::string_view source_package,
                           std::string_view class_name) {
  const std::string local = qualified_name(source_package, class_name);
  return qualified_name(owning_package, local);
}

std::string path_key(const std::filesystem::path& path) {
  std::string key = path_to_utf8(path.lexically_normal());
#if defined(_WIN32)
  for (char& character : key) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
#endif
  return key;
}

std::optional<std::filesystem::path> absolute_path(
    const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::path result = std::filesystem::absolute(path, error);
  if (error) {
    return std::nullopt;
  }
  return result.lexically_normal();
}

bool is_within(const std::filesystem::path& root,
               const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(path, root, error);
  if (error || relative.is_absolute()) {
    return false;
  }
  for (const std::filesystem::path& component : relative) {
    if (component == "..") {
      return false;
    }
  }
  return true;
}

bool valid_package_name(std::string_view package_name) {
  if (package_name.empty()) {
    return true;
  }
  std::size_t begin = 0;
  while (begin < package_name.size()) {
    const std::size_t end = package_name.find('.', begin);
    const std::string_view segment = package_name.substr(
        begin, end == std::string_view::npos ? package_name.size() - begin
                                             : end - begin);
    if (!is_valid_identifier(segment)) {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return true;
}

std::optional<std::string> package_from_path(
    const std::filesystem::path& source_root,
    const std::filesystem::path& source_path) {
  std::error_code error;
  const std::filesystem::path relative =
      std::filesystem::relative(source_path, source_root, error);
  if (error || relative.is_absolute()) {
    return std::nullopt;
  }
  std::string package_name;
  for (const std::filesystem::path& component : relative.parent_path()) {
    if (component == ".") {
      continue;
    }
    if (component == "..") {
      return std::nullopt;
    }
    const std::string segment = path_to_utf8(component);
    if (!is_valid_identifier(segment)) {
      return std::nullopt;
    }
    if (!package_name.empty()) {
      package_name += '.';
    }
    package_name += segment;
  }
  return package_name;
}

std::filesystem::path package_directory(
    const std::filesystem::path& source_root, std::string_view package_name) {
  std::filesystem::path directory = source_root;
  std::size_t begin = 0;
  while (begin < package_name.size()) {
    const std::size_t end = package_name.find('.', begin);
    directory /= std::string{package_name.substr(
        begin, end == std::string_view::npos ? package_name.size() - begin
                                             : end - begin)};
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return directory;
}

SourceRange source_origin(const SourceFile& source) {
  return point_range(SourceLocation{source.display_path(), 0, 1, 1});
}

}  // namespace

Compilation::Compilation() : Compilation(TargetDataLayout::llvm_x86_64()) {}

Compilation::Compilation(TargetDataLayout target)
    : target_(std::move(target)) {}

void Compilation::set_source_root(std::filesystem::path source_root,
                                  bool discover_package_sources) {
  source_root_ = std::move(source_root);
  discover_package_sources_ = discover_package_sources;
}

void Compilation::set_package_dependencies(
    std::vector<CompilationDependency> dependencies) {
  package_dependencies_ = std::move(dependencies);
}

void Compilation::add_source(SourceFile source, std::string package_name) {
  units_.push_back(Unit{
      std::move(source), std::move(package_name), {}, {}, {}, std::nullopt});
}

void Compilation::add_package_source(SourceFile source,
                                     std::string owning_package,
                                     std::string source_package,
                                     std::string package_version) {
  units_.push_back(Unit{std::move(source),
                        std::move(source_package),
                        std::move(owning_package),
                        {},
                        {},
                        std::nullopt,
                        std::move(package_version)});
}

void Compilation::add_imported_package(ImportedPackageView package) {
  imported_packages_.push_back(std::move(package));
}

void Compilation::prepare_source_graph(DiagnosticEngine& diagnostics) {
  std::optional<std::filesystem::path> normalized_source_root;
  if (source_root_) {
    normalized_source_root = absolute_path(*source_root_);
    if (!normalized_source_root) {
      const SourceLocation origin{"<project>", 0, 1, 1};
      diagnostics.error(origin, "could not resolve the project source root");
    }
  }

  std::set<std::string> loaded_paths;
  for (const Unit& unit : units_) {
    if (const auto path = absolute_path(unit.source.path())) {
      loaded_paths.insert(path_key(*path));
    }
  }
  std::set<std::string> loaded_packages;
  std::map<std::string, std::string> package_versions;
  std::map<std::string, ConstantParseBudget> constant_budgets;

  auto prepare_unit = [&](Unit& unit) {
    if (!unit.owning_package.empty()) {
      if (!is_valid_package_name(unit.owning_package) ||
          !is_valid_package_version(unit.package_version)) {
        diagnostics.error(source_origin(unit.source),
                          "source has an invalid owning package identity");
      }
      const auto [previous, inserted] =
          package_versions.emplace(unit.owning_package, unit.package_version);
      if (!inserted && previous->second != unit.package_version) {
        diagnostics.error(source_origin(unit.source),
                          "multiple versions of owning package '" +
                              unit.owning_package + "'");
      }
      if (!valid_package_name(unit.package_name)) {
        diagnostics.error(source_origin(unit.source),
                          "source has an invalid package name");
      }
    } else if (!unit.package_version.empty()) {
      diagnostics.error(
          source_origin(unit.source),
          "standalone source has a package version without an owner");
    } else if (normalized_source_root) {
      const auto path = absolute_path(unit.source.path());
      if (!path || !is_within(*normalized_source_root, *path)) {
        diagnostics.error(source_origin(unit.source),
                          "source file is outside the project source root");
      } else if (path->extension() != ".co") {
        diagnostics.error(source_origin(unit.source),
                          "Cloth source files must use the '.co' extension");
      } else if (const auto package =
                     package_from_path(*normalized_source_root, *path)) {
        unit.package_name = *package;
      } else {
        diagnostics.error(source_origin(unit.source),
                          "source path contains an invalid package name");
      }
    } else if (!valid_package_name(unit.package_name)) {
      diagnostics.error(source_origin(unit.source),
                        "source has an invalid package name");
    }

    unit.qualified_name = qualified_name(unit.owning_package, unit.package_name,
                                         unit.source.stem());
    unit.tokens = Lexer{unit.source, diagnostics}.lex();
    unit.parse_result.emplace(Parser{unit.source, unit.tokens, diagnostics,
                                     &constant_budgets[unit.owning_package]}
                                  .parse());
    unit.parse_result->file_class.package_name = unit.package_name;
    unit.parse_result->file_class.qualified_name = unit.qualified_name;
    unit.parse_result->file_class.owning_package = unit.owning_package;
    unit.parse_result->file_class.owning_package_version = unit.package_version;
    for (ImportDecl& import : unit.parse_result->file_class.imports) {
      import.target_package = unit.owning_package;
      if (unit.owning_package.empty() || import.package_name.empty()) {
        continue;
      }
      const std::size_t separator = import.package_name.find('.');
      const std::string_view first =
          std::string_view{import.package_name}.substr(
              0, separator == std::string::npos ? import.package_name.size()
                                                : separator);
      const auto dependency = std::ranges::find_if(
          package_dependencies_, [&](const CompilationDependency& edge) {
            return edge.owner == unit.owning_package && edge.alias == first;
          });
      if (dependency == package_dependencies_.end()) {
        continue;
      }
      import.target_package = dependency->target;
      import.package_name = separator == std::string::npos
                                ? std::string{}
                                : import.package_name.substr(separator + 1);
    }
  };

  auto load_source = [&](const std::filesystem::path& path, SourceRange range,
                         std::string_view description, bool report_failure) {
    const auto absolute = absolute_path(path);
    if (!absolute) {
      if (report_failure) {
        diagnostics.error(range,
                          "could not resolve " + std::string{description});
      }
      return;
    }
    const std::string key = path_key(*absolute);
    if (loaded_paths.contains(key)) {
      return;
    }
    auto source = SourceFile::load(*absolute);
    if (!source) {
      if (report_failure) {
        diagnostics.error(range, "could not load " + std::string{description} +
                                     " at '" + path_to_utf8(*absolute) + "'");
      }
      return;
    }
    loaded_paths.insert(key);
    units_.push_back(Unit{std::move(*source), {}, {}, {}, {}, std::nullopt});
  };

  auto load_package = [&](std::string_view package_name, SourceRange range) {
    const std::string package{package_name};
    if (!normalized_source_root || loaded_packages.contains(package)) {
      return;
    }
    loaded_packages.insert(package);
    const std::filesystem::path directory =
        package_directory(*normalized_source_root, package_name);
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error) || error) {
      return;
    }

    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator iterator{directory, error};
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
      const std::filesystem::directory_entry& entry = *iterator;
      if (entry.is_regular_file(error) && !error &&
          entry.path().extension() == ".co") {
        paths.push_back(entry.path());
      }
      iterator.increment(error);
    }
    if (error) {
      diagnostics.error(range, "could not enumerate package '" + package + "'");
      return;
    }
    std::ranges::sort(paths, {}, [](const std::filesystem::path& path) {
      return path_to_utf8(path);
    });
    for (const std::filesystem::path& path : paths) {
      load_source(path, range, "source file", true);
    }
  };

  for (std::size_t index = 0; index < units_.size(); ++index) {
    prepare_unit(units_[index]);
    if (!normalized_source_root) {
      continue;
    }

    const std::string current_package = units_[index].package_name;
    const SourceRange origin = source_origin(units_[index].source);
    const std::vector<ImportDecl> imports =
        units_[index].parse_result->file_class.imports;
    if (discover_package_sources_) {
      load_package(current_package, origin);
    }
    for (const ImportDecl& import : imports) {
      if (!import.is_valid) {
        continue;
      }
      if (import.kind == ImportKind::kWildcard) {
        load_package(import.package_name, import.range);
        continue;
      }
      const std::filesystem::path path =
          package_directory(*normalized_source_root, import.package_name) /
          (import.type_name + ".co");
      load_source(path, import.range,
                  "import '" +
                      qualified_name(import.package_name, import.type_name) +
                      "'",
                  false);
    }
  }

  if (normalized_source_root) {
    std::ranges::stable_sort(
        units_, {}, [](const Unit& unit) { return unit.qualified_name; });
  }
}

FrontendResult Compilation::analyze_frontend(DiagnosticEngine& diagnostics) {
  std::vector<const ImportedPackageView*> imported_views;
  for (const auto& package : imported_packages_)
    imported_views.push_back(&package);
  const auto import_issues = verify_imported_package_closure(imported_views);
  if (!import_issues.empty()) {
    for (const auto& issue : import_issues) {
      diagnostics.error(SourceLocation{"<artifact>", 0, 1, 1},
                        issue.record + ": " + issue.message);
    }
    return FrontendResult{SemanticModel{}, {}, {}, false};
  }
  prepare_source_graph(diagnostics);

  std::vector<const FileClassDecl*> files;
  files.reserve(units_.size());
  for (Unit& unit : units_) {
    files.push_back(&unit.parse_result->file_class);
  }

  SemanticAnalysisResult semantic_result =
      analyze_semantics(files, diagnostics, imported_packages_);
  HirModule hir = lower_to_hir(files, semantic_result.model);
  const bool contains_structs = std::ranges::any_of(
      semantic_result.model.files(), [](const FileSemantics& file) {
        return file.kind == FileTypeKind::kStruct;
      });
  const bool contains_switch = std::ranges::any_of(
      hir.storage.statements(), [](const HirStatement& statement) {
        return std::holds_alternative<HirSwitchStatement>(statement.data);
      });
  const bool hir_is_valid =
      (semantic_result.is_valid ||
       (!contains_structs && !contains_switch &&
        !std::ranges::any_of(semantic_result.model.symbols(),
                             [](const SemanticSymbol& symbol) {
                               return symbol.kind == SymbolKind::kField &&
                                      symbol.is_static;
                             }))) &&
      verify_hir(hir, semantic_result.model, diagnostics);
  ControlFlowAnalysis control_flow;
  if (hir_is_valid) {
    control_flow =
        analyze_control_flow(hir, semantic_result.model, diagnostics);
  }
  const bool is_valid =
      semantic_result.is_valid && hir_is_valid && !diagnostics.has_errors();
  return FrontendResult{std::move(semantic_result.model), std::move(hir),
                        std::move(control_flow), is_valid};
}

CompilationResult Compilation::analyze(DiagnosticEngine& diagnostics) {
  FrontendResult frontend = analyze_frontend(diagnostics);
  MirModule mir;
  AbiModule abi{target_, {}, {}};
  bool mir_is_valid = false;
  bool abi_is_valid = false;
  const bool has_structs = std::ranges::any_of(
      frontend.semantics.files(), [](const FileSemantics& file) {
        return file.kind == FileTypeKind::kStruct;
      });
  bool has_switch = false;
  for (const auto& statement : frontend.hir.storage.statements()) {
    if (std::holds_alternative<HirSwitchStatement>(statement.data)) {
      has_switch = true;
      break;
    }
  }
  const bool has_constants = std::ranges::any_of(
      frontend.semantics.symbols(), [](const SemanticSymbol& symbol) {
        return symbol.kind == SymbolKind::kField && symbol.is_static;
      });
  if (frontend.is_valid ||
      (!has_structs && !has_switch && !has_constants &&
       verify_hir(frontend.hir, frontend.semantics, diagnostics))) {
    mir = lower_to_mir(frontend.hir, frontend.semantics);
    for (std::size_t index = mir.files.size();
         index < frontend.semantics.files().size(); ++index) {
      const FileId file_id{index};
      const FileSemantics& semantic_file = frontend.semantics.file(file_id);
      MirFileClass imported{file_id,
                            semantic_file.symbol,
                            semantic_file.base_file,
                            {},
                            {},
                            {},
                            semantic_file.member_order,
                            true};
      imported.fields.reserve(semantic_file.fields.size());
      for (const SymbolId symbol : semantic_file.fields) {
        imported.fields.push_back(
            MirField{symbol, std::nullopt,
                     frontend.semantics.symbol(symbol).static_constant});
      }
      const auto add_callables = [&](std::span<const SymbolId> symbols,
                                     std::vector<MirCallable>& output) {
        output.reserve(symbols.size());
        for (const SymbolId symbol : symbols) {
          const SemanticSymbol& semantic = frontend.semantics.symbol(symbol);
          output.push_back(MirCallable{
              symbol, semantic.parameter_symbols,
              MirBody{semantic.range, MirBlockId{0}, {}, 0},
              semantic_file.kind != FileTypeKind::kStruct
                  ? StructReceiverMode::kNone
              : semantic.kind == SymbolKind::kConstructor
                  ? StructReceiverMode::kConstruction
              : semantic.is_static ? StructReceiverMode::kNone
                                   : StructReceiverMode::kReadOnlyValue});
        }
      };
      add_callables(semantic_file.functions, imported.functions);
      add_callables(semantic_file.constructors, imported.constructors);
      mir.files.push_back(std::move(imported));
    }
    mir_is_valid = verify_mir(mir, frontend.semantics, diagnostics);
    if (mir_is_valid) {
      optimize_mir(mir, frontend.semantics);
      mir_is_valid = verify_mir(mir, frontend.semantics, diagnostics);
    }
    if (mir_is_valid) {
      auto lowered =
          lower_to_abi(mir, frontend.semantics, target_, diagnostics);
      if (lowered) {
        abi = std::move(*lowered);
        abi_is_valid = verify_abi(abi, mir, frontend.semantics, diagnostics);
      }
    }
  }
  const bool is_valid = !diagnostics.has_errors() && frontend.is_valid &&
                        mir_is_valid && abi_is_valid;
  return CompilationResult{std::move(frontend.semantics),
                           std::move(frontend.hir),
                           std::move(frontend.control_flow),
                           std::move(mir),
                           std::move(abi),
                           is_valid};
}

std::size_t Compilation::source_count() const noexcept { return units_.size(); }

const SourceFile& Compilation::source(std::size_t index) const {
  return units_.at(index).source;
}

std::span<const Token> Compilation::tokens(std::size_t index) const {
  return units_.at(index).tokens;
}

const FileClassDecl& Compilation::syntax(std::size_t index) const {
  return units_.at(index).parse_result.value().file_class;
}

}  // namespace cloth
