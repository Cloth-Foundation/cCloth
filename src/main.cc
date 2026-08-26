#include "cloth/abi/abi_printer.h"
#include "cloth/ast/ast_printer.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir_printer.h"
#include "cloth/lexer/token.h"
#include "cloth/mir/mir_printer.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

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
  for (const auto& diagnostic : engine.diagnostics()) {
    const auto& location = diagnostic.range.begin;
    const auto file =
        location.file.empty() ? std::string_view{"<unknown>"} : location.file;
    std::cerr << file << ':' << location.line << ':' << location.column << ": "
              << cloth::diagnostic_severity_name(diagnostic.severity) << ": "
              << diagnostic.message << '\n';
  }
}

void print_tokens(std::span<const cloth::Token> tokens) {
  std::cout << "\nTokens:\n";

  for (const auto& token : tokens) {
    const auto& location = token.range.begin;
    std::ostringstream position;
    position << location.line << ':' << location.column;
    std::cout << std::left << std::setw(8) << position.str() << std::setw(24)
              << cloth::token_kind_name(token.kind)
              << escaped_lexeme(token.lexeme) << '\n';
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "usage: clothc [--target=x86_64|wasm32] "
                 "[--emit-llvm[=<path>] | --build=<path>] "
                 "<source.co>...\n";
    return 2;
  }

  cloth::TargetDataLayout target = cloth::TargetDataLayout::llvm_x86_64();
  bool target_was_set = false;
  bool emit_llvm = false;
  std::optional<std::filesystem::path> llvm_output;
  std::optional<std::filesystem::path> native_output;
  std::vector<std::filesystem::path> source_paths;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if (argument.starts_with("--target=")) {
      if (target_was_set) {
        std::cerr << "clothc: error: target was specified more than once\n";
        return 2;
      }
      target_was_set = true;
      const std::string_view name = argument.substr(9);
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
    if (argument.starts_with("--build=")) {
      if (native_output) {
        std::cerr << "clothc: error: native output was specified more than "
                     "once\n";
        return 2;
      }
      const std::string_view path = argument.substr(8);
      if (path.empty()) {
        std::cerr << "clothc: error: native output path is empty\n";
        return 2;
      }
      native_output.emplace(path);
      continue;
    }
    if (argument == "--emit-llvm" || argument.starts_with("--emit-llvm=")) {
      if (emit_llvm) {
        std::cerr
            << "clothc: error: LLVM output was specified more than once\n";
        return 2;
      }
      emit_llvm = true;
      if (argument.starts_with("--emit-llvm=")) {
        const std::string_view path = argument.substr(12);
        if (path.empty()) {
          std::cerr << "clothc: error: LLVM output path is empty\n";
          return 2;
        }
        llvm_output.emplace(path);
      }
      continue;
    }
    if (argument.starts_with('-')) {
      std::cerr << "clothc: error: unknown option '" << argument << "'\n";
      return 2;
    }
    source_paths.emplace_back(argument);
  }
  if (source_paths.empty()) {
    std::cerr << "clothc: error: no source files were provided\n";
    return 2;
  }
  if (emit_llvm && native_output) {
    std::cerr << "clothc: error: --emit-llvm and --build are mutually "
                 "exclusive\n";
    return 2;
  }
  if (native_output && target.target_name != "x86_64-unknown-unknown") {
    std::cerr << "clothc: error: native executable output currently supports "
                 "only --target=x86_64\n";
    return 2;
  }

  cloth::Compilation compilation{std::move(target)};
  for (const std::filesystem::path& source_path : source_paths) {
    auto source_result = cloth::SourceFile::load(source_path);
    if (!source_result) {
      const auto& error = source_result.error();
      std::cerr << error.path.generic_string() << ": error: " << error.message
                << '\n';
      return 2;
    }
    compilation.add_source(std::move(*source_result));
  }

  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult compilation_result =
      compilation.analyze(diagnostics);

  std::optional<cloth::LlvmIrModule> llvm_module;
  if ((emit_llvm || native_output) && compilation_result.is_valid) {
    llvm_module =
        cloth::emit_llvm_ir(compilation_result.mir, compilation_result.abi,
                            compilation_result.semantics, diagnostics,
                            cloth::LlvmIrOptions{native_output.has_value()});
  }

  print_diagnostics(diagnostics);
  if ((emit_llvm || native_output) && diagnostics.has_errors()) {
    return 1;
  }
  if (native_output) {
    if (!llvm_module) {
      return 1;
    }
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
    const auto build_result =
        cloth::build_native_executable(*llvm_module, *native_output, toolchain);
    if (!build_result) {
      std::cerr << "clothc: error: " << build_result.error().message << '\n';
      return 2;
    }
    return 0;
  }
  if (emit_llvm) {
    if (!llvm_module) {
      return 1;
    }
    if (llvm_output) {
      std::ofstream output{*llvm_output, std::ios::binary};
      if (!output) {
        std::cerr << llvm_output->generic_string()
                  << ": error: could not open LLVM output\n";
        return 2;
      }
      output << llvm_module->text;
      if (!output) {
        std::cerr << llvm_output->generic_string()
                  << ": error: could not write LLVM output\n";
        return 2;
      }
    } else {
      std::cout << llvm_module->text;
    }
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
  cloth::print_hir_summary(compilation_result.hir, compilation_result.semantics,
                           std::cout);
  std::cout << "\nControl-flow MIR:\n";
  cloth::print_mir_summary(compilation_result.mir, compilation_result.semantics,
                           std::cout);
  std::cout << "\nPortable ABI:\n";
  cloth::print_abi_summary(compilation_result.abi, compilation_result.semantics,
                           std::cout);
  return diagnostics.has_errors() ? 1 : 0;
}
