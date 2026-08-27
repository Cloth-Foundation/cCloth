#include "cloth/compiler/compilation.h"

#include "cloth/abi/abi.h"
#include "cloth/abi/abi_verifier.h"
#include "cloth/flow/control_flow.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/lexer/lexer.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/parser/parser.h"
#include "cloth/parser/syntax_facts.h"
#include "cloth/sema/semantic_analyzer.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
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

std::string path_key(const std::filesystem::path& path) {
  std::string key = path.lexically_normal().generic_string();
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
    const std::string segment = component.generic_string();
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

void Compilation::add_source(SourceFile source, std::string package_name) {
  units_.push_back(
      Unit{std::move(source), std::move(package_name), {}, {}, std::nullopt});
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

  auto prepare_unit = [&](Unit& unit) {
    if (normalized_source_root) {
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

    unit.qualified_name = qualified_name(unit.package_name, unit.source.stem());
    unit.tokens = Lexer{unit.source, diagnostics}.lex();
    unit.parse_result.emplace(
        Parser{unit.source, unit.tokens, diagnostics}.parse());
    unit.parse_result->file_class.package_name = unit.package_name;
    unit.parse_result->file_class.qualified_name = unit.qualified_name;
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
                                     " at '" + absolute->generic_string() +
                                     "'");
      }
      return;
    }
    loaded_paths.insert(key);
    units_.push_back(Unit{std::move(*source), {}, {}, {}, std::nullopt});
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
      return path.generic_string();
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

CompilationResult Compilation::analyze(DiagnosticEngine& diagnostics) {
  prepare_source_graph(diagnostics);

  std::vector<const FileClassDecl*> files;
  files.reserve(units_.size());
  for (Unit& unit : units_) {
    files.push_back(&unit.parse_result->file_class);
  }

  SemanticAnalysisResult semantic_result =
      analyze_semantics(files, diagnostics);
  HirModule hir = lower_to_hir(files, semantic_result.model);
  const bool hir_is_valid = verify_hir(hir, semantic_result.model, diagnostics);
  ControlFlowAnalysis control_flow;
  MirModule mir;
  AbiModule abi{target_, {}, {}};
  bool mir_is_valid = false;
  bool abi_is_valid = false;
  if (hir_is_valid) {
    control_flow =
        analyze_control_flow(hir, semantic_result.model, diagnostics);
    mir = lower_to_mir(hir, semantic_result.model);
    mir_is_valid = verify_mir(mir, semantic_result.model, diagnostics);
    if (mir_is_valid) {
      abi = lower_to_abi(mir, semantic_result.model, target_);
      abi_is_valid = verify_abi(abi, mir, semantic_result.model, diagnostics);
    }
  }
  const bool is_valid = !diagnostics.has_errors() && hir_is_valid &&
                        mir_is_valid && abi_is_valid &&
                        semantic_result.is_valid;
  return CompilationResult{std::move(semantic_result.model),
                           std::move(hir),
                           std::move(control_flow),
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
