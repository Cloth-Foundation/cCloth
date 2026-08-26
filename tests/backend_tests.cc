#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/source/source_file.h"
#include "cloth/target/data_layout.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class TestContext {
 public:
  explicit TestContext(std::string_view name) : name_(name) {}

  void expect(bool condition, std::string_view message) {
    if (!condition) {
      ++failures_;
      std::cerr << "  " << name_ << ": " << message << '\n';
    }
  }

  [[nodiscard]] int failures() const noexcept { return failures_; }

 private:
  std::string_view name_;
  int failures_{0};
};

class CompiledSources {
 public:
  explicit CompiledSources(
      cloth::TargetDataLayout target = cloth::TargetDataLayout::llvm_x86_64())
      : compilation_(std::move(target)) {}

  void add(std::filesystem::path path, std::string text) {
    compilation_.add_source(
        cloth::SourceFile::from_memory(std::move(path), std::move(text)));
  }

  void compile(cloth::LlvmIrOptions options = {}) {
    result.emplace(compilation_.analyze(diagnostics));
    if (result->is_valid) {
      llvm = cloth::emit_llvm_ir(result->mir, result->abi, result->semantics,
                                 diagnostics, options);
    }
  }

  [[nodiscard]] bool contains(std::string_view text) const {
    return llvm && llvm->text.find(text) != std::string::npos;
  }

  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;
  std::optional<cloth::LlvmIrModule> llvm;

 private:
  cloth::Compilation compilation_;
};

bool has_diagnostic(const cloth::DiagnosticEngine& diagnostics,
                    std::string_view text) {
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void target_header(TestContext& test) {
  CompiledSources sources;
  sources.add("Empty.co", "");
  sources.compile();

  test.expect(sources.llvm.has_value(), "valid module did not emit LLVM IR");
  test.expect(sources.contains("target triple = \"x86_64-unknown-unknown\""),
              "LLVM target triple is missing");
  test.expect(sources.contains("target datalayout = "),
              "LLVM data layout is missing");
  test.expect(sources.contains("declare ptr @cloth_rt_alloc(i64, i64)"),
              "allocation runtime boundary is missing");
  test.expect(sources.contains("declare void @cloth_rt_print_i32(i32)"),
              "int32 print runtime boundary is missing");
  test.expect(sources.contains("declare void @cloth_rt_print_bool(i8)"),
              "bool print runtime boundary is missing");
}

void arithmetic_and_control_flow(TestContext& test) {
  CompiledSources sources;
  sources.add("Logic.co",
              "func Choose(bool left, bool right, int value): int {\n"
              "  int result = value + 2 * 3;\n"
              "  if (left && right) { return -result; }\n"
              "  return result;\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "control-flow module failed to emit");
  test.expect(
      sources.contains(" = mul i32 2, 3") && sources.contains(" = add i32 "),
      "integer arithmetic was not lowered");
  test.expect(sources.contains(" = phi i1 ") && sources.contains("br i1 "),
              "short-circuit control flow was not lowered");
  test.expect(sources.contains(" = sub i32 0, "),
              "integer negation was not lowered");
}

void object_construction(TestContext& test) {
  CompiledSources sources;
  sources.add("Model.co",
              "String Name = \"default\";\n"
              "int32 Count = 1;\n"
              "Model(String name) { Name = name; }\n"
              "func Get(): String { return Name; }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "object module failed to emit");
  test.expect(sources.contains("@.cloth.str.0 = private unnamed_addr constant"),
              "string literal global is missing");
  test.expect(sources.contains("define internal ptr @_C1I5_Model4_Name") &&
                  sources.contains("call ptr @_C1I5_Model4_Name(ptr %self)"),
              "field initializer was not composed with construction");
  test.expect(sources.contains("call ptr @cloth_rt_alloc(i64 32, i64 8)"),
              "constructor does not allocate its ABI object size");
  test.expect(sources.contains("getelementptr i8, ptr %self, i64 16"),
              "field access does not use its verified ABI offset");
}

void call_receivers(TestContext& test) {
  CompiledSources sources;
  sources.add("User.co",
              "User() {}\n"
              "func Echo(int value): int { return value; }\n"
              "func Forward(int value): int { return Echo(value); }\n");
  sources.add("Calls.co",
              "func Static(): int { return User.Echo(1); }\n"
              "func Instance(User user): int { return user.Echo(2); }\n"
              "func Make(): User { return User(); }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "call module failed to emit");
  test.expect(
      sources.contains("call i32 @_C1F4_User4_EchoP1_i32(ptr null, i32 1)"),
      "class-qualified call did not pass a null receiver");
  test.expect(
      sources.contains("call i32 @_C1F4_User4_EchoP1_i32(ptr %receiver"),
      "unqualified call did not forward its receiver");
  test.expect(
      sources.contains("call i32 @_C1F4_User4_EchoP1_i32(ptr %v0, i32 2)"),
      "instance-qualified call did not pass its object");
  test.expect(sources.contains("call ptr @_C1C4_User4_UserP0()"),
              "constructor call gained an ABI receiver");
}

void wasm32_module(TestContext& test) {
  CompiledSources sources{cloth::TargetDataLayout::llvm_wasm32()};
  sources.add("Small.co", "int32 Value;\nSmall() {}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "wasm32 module failed to emit");
  test.expect(sources.contains("target triple = \"wasm32-unknown-unknown\""),
              "wasm32 triple is missing");
  test.expect(sources.contains("call ptr @cloth_rt_alloc(i64 12, i64 4)"),
              "wasm32 object size was not preserved");
}

void print_and_native_entry_point(TestContext& test) {
  CompiledSources sources;
  sources.add("HelloWorld.co",
              "func Main() {\n"
              "  print(\"Hello, World!\\n\");\n"
              "  print(7);\n"
              "  print(true);\n"
              "}\n");
  sources.compile(cloth::LlvmIrOptions{true});

  test.expect(sources.llvm.has_value(), "native entry module failed to emit");
  test.expect(sources.contains("declare void @cloth_rt_print(ptr)"),
              "print runtime boundary is missing");
  test.expect(sources.contains("call void @cloth_rt_print(ptr %v0)"),
              "String print intrinsic was not lowered");
  test.expect(sources.contains("call void @cloth_rt_print_i32(i32 7)"),
              "int32 print intrinsic was not lowered");
  test.expect(sources.contains("zext i1 true to i8") &&
                  sources.contains("call void @cloth_rt_print_bool(i8 %addr"),
              "bool print intrinsic was not lowered with its ABI width");
  test.expect(
      sources.contains("define i32 @main()") &&
          sources.contains("call void @_C1F10_HelloWorld4_MainP0(ptr null)"),
      "native entry adapter was not emitted");

  CompiledSources exit_code;
  exit_code.add("Program.co", "func Main(): int32 { return 7; }\n");
  exit_code.compile(cloth::LlvmIrOptions{true});
  test.expect(exit_code.contains(
                  "%exit_code = call i32 @_C1F7_Program4_MainP0(ptr null)") &&
                  exit_code.contains("ret i32 %exit_code"),
              "int32 Main did not supply the process exit code");
}

void rejects_invalid_native_entry_points(TestContext& test) {
  CompiledSources missing;
  missing.add("Library.co", "func Read(): int { return 1; }\n");
  missing.compile(cloth::LlvmIrOptions{true});
  test.expect(
      !missing.llvm && has_diagnostic(missing.diagnostics,
                                      "requires a public 'Main' function"),
      "missing native entry point was accepted");

  CompiledSources invalid;
  invalid.add("Program.co", "func Main(int value): int { return value; }\n");
  invalid.compile(cloth::LlvmIrOptions{true});
  test.expect(
      !invalid.llvm && has_diagnostic(invalid.diagnostics,
                                      "entry point 'Main' must be public"),
      "invalid native entry signature was accepted");

  CompiledSources duplicate;
  duplicate.add("First.co", "func Main() {}\n");
  duplicate.add("Second.co", "func Main(): int { return 0; }\n");
  duplicate.compile(cloth::LlvmIrOptions{true});
  test.expect(
      !duplicate.llvm && has_diagnostic(duplicate.diagnostics,
                                        "more than one eligible 'Main'"),
      "duplicate native entry points were accepted");
}

void rejects_inconsistent_input(TestContext& test) {
  CompiledSources sources;
  sources.add("Broken.co", "func Value(): int { return 1; }\n");
  sources.compile();
  cloth::AbiModule broken = sources.result->abi;
  broken.files.clear();
  cloth::DiagnosticEngine diagnostics;
  const auto llvm = cloth::emit_llvm_ir(sources.result->mir, broken,
                                        sources.result->semantics, diagnostics);

  test.expect(!llvm, "LLVM emitter accepted inconsistent MIR and ABI");
  test.expect(has_diagnostic(diagnostics, "MIR and ABI file counts differ"),
              "LLVM emitter reported the wrong invariant failure");
}

void rejects_out_of_range_literal(TestContext& test) {
  CompiledSources sources;
  sources.add("Huge.co",
              "func Value(): int { return 999999999999999999999; }\n");
  sources.compile();

  test.expect(!sources.llvm,
              "LLVM emitter accepted an out-of-range integer literal");
  test.expect(
      has_diagnostic(sources.diagnostics, "integer literal is out of range"),
      "out-of-range literal produced the wrong diagnostic");
}

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"target header", target_header},
      {"arithmetic and control flow", arithmetic_and_control_flow},
      {"object construction", object_construction},
      {"call receivers", call_receivers},
      {"wasm32 module", wasm32_module},
      {"print and native entry point", print_and_native_entry_point},
      {"rejects invalid native entry points",
       rejects_invalid_native_entry_points},
      {"rejects inconsistent input", rejects_inconsistent_input},
      {"rejects out-of-range literal", rejects_out_of_range_literal},
  };

  int failures = 0;
  for (const TestCase& test_case : tests) {
    TestContext context{test_case.name};
    test_case.function(context);
    if (context.failures() == 0) {
      std::cout << "[pass] " << test_case.name << '\n';
    } else {
      std::cout << "[fail] " << test_case.name << '\n';
      failures += context.failures();
    }
  }

  if (failures == 0) {
    std::cout << tests.size() << " tests passed\n";
    return 0;
  }
  std::cerr << failures << " assertion(s) failed\n";
  return 1;
}
