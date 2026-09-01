// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_printer.h"
#include "cloth/ast/ast_printer.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/compilation.h"
#include "cloth/compiler/shuttle_protocol.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir_printer.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir_printer.h"
#include "cloth/source/path.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
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

namespace {

struct OutputOptions {
  bool protocol_mode{false};
  bool emit_llvm{false};
  std::optional<std::filesystem::path> llvm_output;
  std::optional<std::filesystem::path> native_output;
  std::optional<std::string> entry_file;
};

bool argument_starts_with(const std::filesystem::path& argument,
                          std::string_view prefix) {
  const auto& native = argument.native();
  if (native.size() < prefix.size()) {
    return false;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    if (native[index] != prefix[index]) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> ascii_argument(
    const std::filesystem::path& argument) {
  const auto& native = argument.native();
  std::string result;
  result.reserve(native.size());
  for (const auto character : native) {
    const auto value = static_cast<std::uint32_t>(character);
    if (value > 0x7f) {
      return std::nullopt;
    }
    result.push_back(static_cast<char>(value));
  }
  return result;
}

std::filesystem::path argument_suffix(const std::filesystem::path& argument,
                                      std::size_t prefix_size) {
  return std::filesystem::path{argument.native().substr(prefix_size)};
}

std::string escaped_lexeme(std::string_view lexeme) {
  std::ostringstream output;
  output << '"';
  for (const char character : lexeme) {
    switch (character) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      case '\0':
        output << "\\0";
        break;
      default: {
        const auto value = static_cast<unsigned char>(character);
        if (std::isprint(value) != 0) {
          output << character;
        } else {
          output << "\\x" << std::uppercase << std::hex << std::setw(2)
                 << std::setfill('0') << static_cast<unsigned int>(value)
                 << std::dec << std::nouppercase << std::setfill(' ');
        }
        break;
      }
    }
  }
  output << '"';
  return output.str();
}

void print_diagnostics(const cloth::DiagnosticEngine& engine) {
  for (const cloth::Diagnostic& diagnostic : engine.diagnostics()) {
    const cloth::SourceLocation& location = diagnostic.range.begin;
    const std::string_view file =
        location.file.empty() ? std::string_view{"<unknown>"} : location.file;
    std::cerr << file << ':' << location.line << ':' << location.column << ": "
              << cloth::diagnostic_severity_name(diagnostic.severity) << ": "
              << diagnostic.message << '\n';
  }
}

void print_tokens(std::span<const cloth::Token> tokens) {
  std::cout << "\nTokens:\n";
  for (const cloth::Token& token : tokens) {
    const cloth::SourceLocation& location = token.range.begin;
    std::ostringstream position;
    position << location.line << ':' << location.column;
    std::cout << std::left << std::setw(8) << position.str() << std::setw(24)
              << cloth::token_kind_name(token.kind)
              << escaped_lexeme(token.lexeme) << '\n';
  }
}

cloth::NativeToolchain native_toolchain() {
  cloth::NativeToolchain toolchain;
#if defined(CLOTH_DEFAULT_LLC)
  toolchain.llc = CLOTH_DEFAULT_LLC;
#endif
  toolchain.linker = CLOTH_DEFAULT_NATIVE_LINKER;
  toolchain.runtime_library = CLOTH_DEFAULT_RUNTIME_LIBRARY;
  toolchain.target_triple = CLOTH_DEFAULT_NATIVE_TARGET;
#if defined(CLOTH_NATIVE_LINKER_MSVC)
  toolchain.linker_flavor = cloth::NativeLinkerFlavor::kMsvc;
#endif
#if defined(CLOTH_NATIVE_STATIC_RUNTIME)
  toolchain.link_static_runtime = true;
#endif
  return toolchain;
}

class TemporaryOutput {
 public:
  explicit TemporaryOutput(const std::filesystem::path& output) {
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 16; ++attempt) {
      directory_ =
          output.parent_path() / (".cloth-output." + std::to_string(ticks) +
                                  "." + std::to_string(sequence.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(directory_, error)) {
        path_ = directory_ / "completed";
        ready_ = true;
        return;
      }
      if (error) {
        break;
      }
    }
    std::cerr << cloth::path_to_utf8(output)
              << ": error: could not create private output directory\n";
  }

  ~TemporaryOutput() {
    if (ready_) {
      std::error_code error;
      std::filesystem::remove(path_, error);
      error.clear();
      std::filesystem::remove(directory_, error);
    }
  }

  TemporaryOutput(const TemporaryOutput&) = delete;
  TemporaryOutput& operator=(const TemporaryOutput&) = delete;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
  bool ready_ = false;
};

bool promote_output(const std::filesystem::path& temporary,
                    const std::filesystem::path& output) {
  std::error_code error;
#if defined(_WIN32)
  if (!MoveFileExW(temporary.c_str(), output.c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
    error = std::error_code{static_cast<int>(GetLastError()),
                            std::system_category()};
  }
#else
  std::filesystem::rename(temporary, output, error);
#endif
  if (error) {
    std::cerr << cloth::path_to_utf8(output)
              << ": error: could not install completed output\n";
    return false;
  }
  return true;
}

bool write_llvm_module(const cloth::LlvmIrModule& module,
                       const std::filesystem::path& output) {
  const TemporaryOutput temporary{output};
  if (!temporary.ready()) {
    return false;
  }
  std::ofstream stream{temporary.path(), std::ios::binary};
  if (!stream) {
    std::cerr << cloth::path_to_utf8(output)
              << ": error: could not open LLVM output\n";
    return false;
  }
  stream << module.text;
  stream.close();
  if (!stream) {
    std::cerr << cloth::path_to_utf8(output)
              << ": error: could not write LLVM output\n";
    return false;
  }
  return promote_output(temporary.path(), output);
}

int compile(cloth::Compilation& compilation, const OutputOptions& options) {
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);

  if (result.is_valid && options.entry_file && !options.native_output) {
    static_cast<void>(cloth::find_native_entry_point(
        result.abi, result.semantics, diagnostics, *options.entry_file));
  }

  std::optional<cloth::LlvmIrModule> llvm_module;
  if ((options.emit_llvm || options.native_output) && result.is_valid &&
      !diagnostics.has_errors()) {
    llvm_module = cloth::emit_llvm_ir(
        result.mir, result.abi, result.semantics, diagnostics,
        cloth::LlvmIrOptions{options.native_output.has_value(),
                             options.entry_file});
  }
  print_diagnostics(diagnostics);
  if (diagnostics.has_errors() || !result.is_valid) {
    return 1;
  }

  if (options.native_output) {
    if (!llvm_module) {
      return 1;
    }
    const TemporaryOutput temporary{*options.native_output};
    if (!temporary.ready()) {
      return 2;
    }
    const auto build = cloth::build_native_executable(
        *llvm_module, temporary.path(), native_toolchain());
    if (!build) {
      std::cerr << "clothc: error: " << build.error().message << '\n';
      return 2;
    }
    return promote_output(temporary.path(), *options.native_output) ? 0 : 2;
  }
  if (options.emit_llvm) {
    if (!llvm_module) {
      return 1;
    }
    if (options.llvm_output) {
      return write_llvm_module(*llvm_module, *options.llvm_output) ? 0 : 2;
    }
    std::cout << llvm_module->text;
    return 0;
  }
  if (options.protocol_mode) {
    return 0;
  }

  for (std::size_t index = 0; index < compilation.source_count(); ++index) {
    if (compilation.source_count() > 1) {
      std::cout << "\nSource " << compilation.source(index).display_path()
                << ":\n";
    }
    print_tokens(compilation.tokens(index));
    std::cout << "\nAST:\n";
    cloth::print_ast_summary(compilation.syntax(index), std::cout);
  }
  std::cout << "\nTyped HIR:\n";
  cloth::print_hir_summary(result.hir, result.semantics, std::cout);
  std::cout << "\nControl-flow MIR:\n";
  cloth::print_mir_summary(result.mir, result.semantics, std::cout);
  std::cout << "\nPortable ABI:\n";
  cloth::print_abi_summary(result.abi, result.semantics, std::cout);
  return result.is_valid ? 0 : 1;
}

int run_protocol(std::span<const std::filesystem::path> arguments) {
  auto plan = cloth::prepare_shuttle_build(arguments);
  if (!plan) {
    std::cerr << "clothc: error: " << plan.error() << '\n';
    return 2;
  }

  cloth::Compilation compilation{plan->request.target};
  std::vector<cloth::CompilationDependency> dependencies;
  dependencies.reserve(plan->request.dependencies.size());
  for (const cloth::ShuttleDependencyInput& dependency :
       plan->request.dependencies) {
    dependencies.push_back(cloth::CompilationDependency{
        dependency.owner, dependency.alias, dependency.target});
  }
  compilation.set_package_dependencies(std::move(dependencies));
  for (cloth::ShuttleSourceInput& source : plan->sources) {
    compilation.add_package_source(
        std::move(source.source), std::move(source.package),
        std::move(source.source_package), std::move(source.version));
  }

  OutputOptions options;
  options.protocol_mode = true;
  options.entry_file = std::move(plan->entry_file);
  switch (plan->request.output_kind) {
    case cloth::ShuttleOutputKind::kCheck:
      break;
    case cloth::ShuttleOutputKind::kLlvmIr:
      options.emit_llvm = true;
      options.llvm_output = std::move(plan->request.output);
      break;
    case cloth::ShuttleOutputKind::kExecutable:
      options.native_output = std::move(plan->request.output);
      break;
  }
  return compile(compilation, options);
}

int run_direct(std::span<const std::filesystem::path> arguments) {
  cloth::TargetDataLayout target = cloth::TargetDataLayout::llvm_x86_64();
  bool target_was_set = false;
  OutputOptions options;
  std::optional<std::filesystem::path> source_root;
  std::vector<std::filesystem::path> source_paths;

  for (const std::filesystem::path& argument : arguments) {
    const std::optional<std::string> text = ascii_argument(argument);
    if (text && text->starts_with("--target=")) {
      if (target_was_set) {
        std::cerr << "clothc: error: target was specified more than once\n";
        return 2;
      }
      target_was_set = true;
      const std::string_view name = std::string_view{*text}.substr(9);
      if (name == "x86_64") {
        target = cloth::TargetDataLayout::llvm_x86_64();
      } else if (name == "wasm32") {
        target = cloth::TargetDataLayout::llvm_wasm32();
      } else {
        std::cerr << "clothc: error: unsupported target '" << name << "'\n";
        return 2;
      }
      continue;
    }
    if (argument_starts_with(argument, "--build=")) {
      if (options.native_output) {
        std::cerr << "clothc: error: native output was specified more than "
                     "once\n";
        return 2;
      }
      const std::filesystem::path path = argument_suffix(argument, 8);
      if (path.empty()) {
        std::cerr << "clothc: error: native output path is empty\n";
        return 2;
      }
      options.native_output = path;
      continue;
    }
    if (text == "--emit-llvm" ||
        argument_starts_with(argument, "--emit-llvm=")) {
      if (options.emit_llvm) {
        std::cerr
            << "clothc: error: LLVM output was specified more than once\n";
        return 2;
      }
      options.emit_llvm = true;
      if (argument_starts_with(argument, "--emit-llvm=")) {
        const std::filesystem::path path = argument_suffix(argument, 12);
        if (path.empty()) {
          std::cerr << "clothc: error: LLVM output path is empty\n";
          return 2;
        }
        options.llvm_output = path;
      }
      continue;
    }
    if (argument_starts_with(argument, "--source-root=")) {
      if (source_root) {
        std::cerr
            << "clothc: error: source root was specified more than once\n";
        return 2;
      }
      const std::filesystem::path path = argument_suffix(argument, 14);
      if (path.empty()) {
        std::cerr << "clothc: error: source root path is empty\n";
        return 2;
      }
      source_root = path;
      continue;
    }
    if (text && text->starts_with('-')) {
      std::cerr << "clothc: error: unknown option '" << *text << "'\n";
      return 2;
    }
    source_paths.push_back(argument);
  }

  if (source_paths.empty()) {
    std::cerr << "clothc: error: no source files were provided\n";
    return 2;
  }
  if (options.emit_llvm && options.native_output) {
    std::cerr << "clothc: error: --emit-llvm and --build are mutually "
                 "exclusive\n";
    return 2;
  }
  if (options.native_output && target.target_name != "x86_64-unknown-unknown") {
    std::cerr << "clothc: error: native executable output currently supports "
                 "only --target=x86_64\n";
    return 2;
  }

  std::error_code error;
  std::filesystem::path root =
      source_root ? std::filesystem::absolute(*source_root, error)
                  : std::filesystem::absolute(source_paths.front(), error)
                        .parent_path();
  root = root.lexically_normal();
  if (error || !std::filesystem::is_directory(root, error) || error) {
    std::cerr << "clothc: error: could not resolve the source root\n";
    return 2;
  }

  cloth::Compilation compilation{std::move(target)};
  compilation.set_source_root(root, source_root.has_value());
  for (const std::filesystem::path& source_path : source_paths) {
    auto source = cloth::SourceFile::load(source_path);
    if (!source) {
      const cloth::SourceLoadError& load_error = source.error();
      std::cerr << cloth::path_to_utf8(load_error.path)
                << ": error: " << load_error.message << '\n';
      return 2;
    }
    compilation.add_source(std::move(*source));
  }
  return compile(compilation, options);
}

int cloth_main(std::span<const std::filesystem::path> arguments) {
  if (arguments.empty()) {
    std::cerr << "usage: clothc [--source-root=<path>] "
                 "[--target=x86_64|wasm32] "
                 "[--emit-llvm[=<path>] | --build=<path>] <source.co>...\n";
    return 2;
  }
  if (arguments.size() == 1 &&
      ascii_argument(arguments.front()) == "--shuttle-protocol-version") {
    std::cout << cloth::kShuttleProtocolVersion << '\n';
    return 0;
  }
  const bool protocol_mode =
      std::ranges::any_of(arguments, [](const std::filesystem::path& argument) {
        return ascii_argument(argument) == "--shuttle-protocol";
      });
  return protocol_mode ? run_protocol(arguments) : run_direct(arguments);
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
  std::vector<std::filesystem::path> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return cloth_main(arguments);
}
