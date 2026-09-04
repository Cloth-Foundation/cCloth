// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_verifier.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/backend/native_toolchain.h"
#include "cloth/compiler/compilation.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/target/data_layout.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct IntegerType {
  std::string_view name;
  bool is_signed;
  unsigned width;
};

struct IntegerValue {
  bool negative;
  std::uint64_t magnitude;
};

constexpr std::array kTypes{
    IntegerType{"int8", true, 8},     IntegerType{"int16", true, 16},
    IntegerType{"int", true, 32},     IntegerType{"int32", true, 32},
    IntegerType{"int64", true, 64},   IntegerType{"byte", false, 8},
    IntegerType{"uint8", false, 8},   IntegerType{"uint16", false, 16},
    IntegerType{"uint", false, 32},   IntegerType{"uint32", false, 32},
    IntegerType{"uint64", false, 64},
};

std::uint64_t mask(unsigned width) {
  return width == 64 ? UINT64_MAX : (std::uint64_t{1} << width) - 1;
}

std::uint64_t positive_maximum(const IntegerType& type) {
  return type.is_signed ? (std::uint64_t{1} << (type.width - 1)) - 1
                        : mask(type.width);
}

bool is_representable(IntegerValue value, const IntegerType& type) {
  if (value.negative) {
    return type.is_signed && value.magnitude != 0 &&
           value.magnitude <= (std::uint64_t{1} << (type.width - 1));
  }
  return value.magnitude <= positive_maximum(type);
}

void add_value(std::vector<IntegerValue>& values, IntegerValue value,
               const IntegerType& source) {
  if (!is_representable(value, source)) return;
  const auto duplicate = std::ranges::find_if(values, [value](IntegerValue x) {
    return x.negative == value.negative && x.magnitude == value.magnitude;
  });
  if (duplicate == values.end()) values.push_back(value);
}

std::vector<IntegerValue> boundary_values(const IntegerType& source,
                                          const IntegerType& target) {
  std::vector<IntegerValue> values;
  const auto add = [&values, &source](IntegerValue value) {
    add_value(values, value, source);
  };
  const auto add_positive_boundary = [&add](std::uint64_t value) {
    if (value > 0) add({false, value - 1});
    add({false, value});
    if (value != UINT64_MAX) add({false, value + 1});
  };
  add({false, 0});
  add({false, 1});
  add_positive_boundary(positive_maximum(source));
  if (source.is_signed) {
    const std::uint64_t minimum = std::uint64_t{1} << (source.width - 1);
    add({true, 1});
    add({true, minimum - 1});
    add({true, minimum});
  }
  add_positive_boundary(positive_maximum(target));
  if (target.is_signed) {
    const std::uint64_t minimum = std::uint64_t{1} << (target.width - 1);
    add({true, minimum + 1});
    if (minimum > 1) add({true, minimum - 1});
    add({true, minimum});
  } else {
    add({true, 1});
  }
  return values;
}

std::string literal(IntegerValue value) {
  return (value.negative ? "-" : "") + std::to_string(value.magnitude);
}

std::string matrix_source() {
  std::string declarations;
  std::string main_body = "static func Main(): int32 {\n";
  std::size_t pair_index = 0;
  std::size_t value_index = 0;
  for (const IntegerType& source : kTypes) {
    for (const IntegerType& target : kTypes) {
      const std::string check = "check" + std::to_string(pair_index++);
      declarations +=
          "static func " + check + "(" + std::string{source.name} + " value, " +
          std::string{target.name} + " wrapped, " + std::string{target.name} +
          " saturated): bool {\n" + "  return " + std::string{target.name} +
          "::wrap(value) == wrapped && " + std::string{target.name} +
          "::sat(value) == saturated;\n}\n";
      for (const IntegerValue value : boundary_values(source, target)) {
        const std::string suffix = std::to_string(value_index++);
        const std::string input = "input" + suffix;
        const std::string wrapped = "wrapped" + suffix;
        const std::string saturated = "saturated" + suffix;
        declarations += "static final " + std::string{source.name} + " " +
                        input + " = " + literal(value) + ";\n" +
                        "static final " + std::string{target.name} + " " +
                        wrapped + " = " + std::string{target.name} + "::wrap(" +
                        input + ");\n" + "static final " +
                        std::string{target.name} + " " + saturated + " = " +
                        std::string{target.name} + "::sat(" + input + ");\n";
        main_body += "  if (!" + check + "(" + input + ", " + wrapped + ", " +
                     saturated + ")) { println(" + suffix + "); return 1; }\n";
      }
    }
  }
  main_body += "  return 0;\n}\n";
  return declarations + main_body;
}

void print_errors(const cloth::DiagnosticEngine& diagnostics) {
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    std::cerr << diagnostic.message << '\n';
  }
}

int compile(std::string_view mode, const char* output_path) {
  const bool native = mode == "native";
  std::optional<cloth::TargetDataLayout> target;
  if (native || mode == "llvm-x86_64") {
    target = cloth::TargetDataLayout::llvm_x86_64();
  } else if (mode == "llvm-wasm32") {
    target = cloth::TargetDataLayout::llvm_wasm32();
  } else {
    return 2;
  }

  cloth::Compilation compilation{std::move(*target)};
  compilation.add_source(
      cloth::SourceFile::from_memory("Main.co", matrix_source()));
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  if (!result.is_valid ||
      !cloth::verify_mir(result.mir, result.semantics, diagnostics) ||
      !cloth::verify_abi(result.abi, result.mir, result.semantics,
                         diagnostics)) {
    print_errors(diagnostics);
    return 1;
  }
  const auto llvm =
      cloth::emit_llvm_ir(result.mir, result.abi, result.semantics, diagnostics,
                          cloth::LlvmIrOptions{true, "Main", std::nullopt});
  if (!llvm) {
    print_errors(diagnostics);
    return 1;
  }
  if (!native) {
    std::ofstream output{output_path, std::ios::binary};
    output << llvm->text;
    return output ? 0 : 1;
  }

#if defined(CLOTH_DEFAULT_LLC)
  cloth::NativeToolchain toolchain{
      CLOTH_DEFAULT_LLC, CLOTH_DEFAULT_NATIVE_LINKER,
      CLOTH_DEFAULT_RUNTIME_LIBRARY, CLOTH_DEFAULT_NATIVE_TARGET};
#if defined(CLOTH_NATIVE_LINKER_MSVC)
  toolchain.linker_flavor = cloth::NativeLinkerFlavor::kMsvc;
#endif
#if defined(CLOTH_NATIVE_STATIC_RUNTIME)
  toolchain.link_static_runtime = true;
#endif
  const auto built =
      cloth::build_native_executable(*llvm, output_path, toolchain);
  if (!built) {
    std::cerr << built.error().message << '\n';
    return 1;
  }
  return 0;
#else
  static_cast<void>(output_path);
  return 2;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  return compile(argv[1], argv[2]);
}
