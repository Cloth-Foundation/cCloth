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

std::size_t count_occurrences(std::string_view text, std::string_view pattern) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(pattern, offset)) != std::string_view::npos) {
    ++count;
    offset += pattern.size();
  }
  return count;
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
  test.expect(sources.contains("declare ptr @cloth_rt_alloc(ptr)"),
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
              "string Name = \"default\";\n"
              "int32 Count = 1;\n"
              "Model(string name) { Name = name; }\n"
              "func Get(): string { return Name; }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "object module failed to emit");
  test.expect(sources.contains("@.cloth.str.0 = private unnamed_addr constant"),
              "string literal global is missing");
  test.expect(
      sources.contains("@.cloth.type.references.0 = private unnamed_addr "
                       "constant [1 x i64] [i64 16]") &&
          sources.contains("@.cloth.type.0 = private constant { i64, ptr, "
                           "i64, i64, i64, ptr, i64 } { i64 0, ptr "
                           "@.cloth.str.0, i64 5, i64 32, i64 8, ptr "
                           "@.cloth.type.references.0, i64 1 }"),
      "file-class type descriptor metadata is missing");
  test.expect(sources.contains("define internal ptr @_C1I5_Model4_Name") &&
                  sources.contains("call ptr @_C1I5_Model4_Name(ptr %self)"),
              "field initializer was not composed with construction");
  test.expect(sources.contains("call ptr @cloth_rt_alloc(ptr @.cloth.type.0)"),
              "constructor does not allocate through its type descriptor");
  test.expect(sources.contains("getelementptr i8, ptr %self, i64 16"),
              "field access does not use its verified ABI offset");
}

void arrays(TestContext& test) {
  CompiledSources sources;
  sources.add("Arrays.co",
              "func Sum(): int32 {\n"
              "  int32[] values = [1, 2, 3];\n"
              "  string[] labels = [\"cloth\"];\n"
              "  values[1] = 4;\n"
              "  return values.Length + values[0];\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "array module failed to emit");
  test.expect(sources.contains(
                  "call ptr @cloth_rt_array_alloc(i32 3, i64 4, i64 4, i8 0)"),
              "array allocation lost its element layout");
  test.expect(sources.contains(
                  "call ptr @cloth_rt_array_alloc(i32 1, i64 8, i64 8, i8 1)"),
              "reference array allocation lost collector metadata");
  test.expect(sources.contains("call ptr @cloth_rt_array_element(ptr ") &&
                  sources.contains("call i32 @cloth_rt_array_length(ptr "),
              "checked array access runtime calls are missing");
  test.expect(sources.contains("store i32 4, ptr %addr"),
              "array element store was not emitted");
}

void strings(TestContext& test) {
  CompiledSources sources;
  sources.add("Strings.co",
              "func Inspect(string left, string right): int32 {\n"
              "  string joined = left + right;\n"
              "  println(joined == \"cloth\");\n"
              "  println(left != right);\n"
              "  println(joined.IsEmpty);\n"
              "  return joined.Length + joined.ByteLength;\n"
              "}\n"
              "func Optional(string? left, string? right): bool {\n"
              "  return left == right;\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "string module failed to emit");
  test.expect(sources.contains("call ptr @cloth_rt_string_concat(ptr "),
              "string concatenation did not use the runtime boundary");
  test.expect(count_occurrences(sources.llvm ? sources.llvm->text : "",
                                "call i8 @cloth_rt_string_equal(ptr ") == 3,
              "string equality did not use content comparison");
  test.expect(
      sources.contains("call i32 @cloth_rt_string_length(ptr ") &&
          sources.contains("call i32 @cloth_rt_string_byte_length(ptr ") &&
          sources.contains("call i8 @cloth_rt_string_is_empty(ptr "),
      "string properties did not use their runtime boundaries");
  test.expect(sources.contains("icmp ne i8 "),
              "runtime string booleans were not converted to LLVM i1");
}

void call_receivers(TestContext& test) {
  CompiledSources sources;
  sources.add("User.co",
              "User() {}\n"
              "static func Echo(int value): int { return value; }\n"
              "func InstanceEcho(int value): int { return value; }\n"
              "func Forward(int value): int { return InstanceEcho(value); }\n");
  sources.add("Calls.co",
              "static func Static(): int { return User.Echo(1); }\n"
              "func Instance(User user): int { return user.InstanceEcho(2); }\n"
              "func Make(): User { return User(); }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "call module failed to emit");
  test.expect(sources.contains("call i32 @_C1F4_User4_EchoP1_i32(i32 1)"),
              "static call gained an ABI receiver");
  test.expect(sources.contains(
                  "call i32 @_C1F4_User12_InstanceEchoP1_i32(ptr %receiver"),
              "unqualified call did not forward its receiver");
  test.expect(sources.contains(
                  "call i32 @_C1F4_User12_InstanceEchoP1_i32(ptr %v0, i32 2)"),
              "instance-qualified call did not pass its object");
  test.expect(sources.contains("call ptr @_C1C4_User4_UserP0()"),
              "constructor call gained an ABI receiver");
}

void static_members(TestContext& test) {
  CompiledSources sources;
  sources.add(
      "Statics.co",
      "static final int32 Version = 12;\n"
      "int32 Value;\n"
      "static func Twice(int32 value): int32 { return value + value; }\n"
      "static func Main(): int32 { "
      "return Statics.Twice(Statics.Version); }\n");
  sources.compile(cloth::LlvmIrOptions{true});

  test.expect(sources.llvm.has_value(), "static member module failed to emit");
  test.expect(
      sources.contains("@_C1S7_Statics7_Version = constant i32 12, align 4"),
      "static field was not emitted as constant global storage");
  test.expect(
      sources.contains("define i32 @_C1F7_Statics5_TwiceP1_i32(i32 %arg"),
      "static function gained a receiver parameter");
  test.expect(
      sources.contains("load i32, ptr @_C1S7_Statics7_Version") &&
          sources.contains("call i32 @_C1F7_Statics5_TwiceP1_i32(i32 %v"),
      "unqualified static call did not use receiver-free ABI");
  test.expect(sources.contains("call i32 @_C1F7_Statics4_MainP0()"),
              "native entry adapter did not call static Main directly");
}

void null_ergonomics(TestContext& test) {
  CompiledSources sources;
  sources.add("User.co", "string Name = \"Ada\";\n");
  sources.add("NullErgonomics.co",
              "func Display(User? user): string {\n"
              "  return user?.Name ?? \"Unknown\";\n"
              "}\n"
              "func Assert(User? user): User { return user!; }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "null ergonomics module failed to emit");
  test.expect(sources.contains("declare void @cloth_rt_require_non_null(ptr)"),
              "non-null assertion runtime boundary is missing");
  test.expect(
      sources.contains(" = icmp ne ptr ") && sources.contains(" = phi ptr "),
      "safe access and coalescing did not emit guarded LLVM flow");
  test.expect(sources.contains("call void @cloth_rt_require_non_null(ptr "),
              "non-null assertion did not emit its runtime guard");
}

void gc_root_frames(TestContext& test) {
  CompiledSources sources;
  sources.add("Rooted.co",
              "string Name;\n"
              "Rooted(string name) { Name = name; }\n"
              "func Choose(Rooted? value, bool keep): Rooted? {\n"
              "  Rooted? local = value;\n"
              "  if (keep) { return local; }\n"
              "  return null;\n"
              "}\n"
              "static func Make(string name): Rooted {\n"
              "  return Rooted(name);\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "GC root module failed to emit");
  test.expect(
      sources.contains("declare void @cloth_rt_gc_push_frame(ptr, ptr, i64)") &&
          sources.contains("declare void @cloth_rt_gc_pop_frame(ptr)"),
      "GC shadow-stack runtime boundary is missing");
  test.expect(sources.contains("%gc.frame = alloca { ptr, ptr, i64 }") &&
                  sources.contains("%gc.roots = alloca ["),
              "GC root frame storage was not emitted");
  test.expect(sources.contains("store ptr %receiver, ptr %gc.receiver") &&
                  sources.contains("store ptr %self, ptr %gc.self") &&
                  sources.contains("store ptr %v") &&
                  sources.contains("ptr %gc.v"),
              "receiver, constructor self, or temporary roots are missing");
  test.expect(
      sources.contains("call void @cloth_rt_gc_push_frame(ptr %gc.frame") &&
          sources.contains("call void @cloth_rt_gc_pop_frame(ptr %gc.frame)\n"
                           "  ret ptr"),
      "GC root frames are not balanced around callable returns");
  test.expect(count_occurrences(sources.llvm->text,
                                "call void @cloth_rt_gc_push_frame(") == 3 &&
                  count_occurrences(sources.llvm->text,
                                    "call void @cloth_rt_gc_pop_frame(") == 4,
              "GC root frames were not emitted once per call and return path");

  CompiledSources scalar;
  scalar.add("Scalar.co", "static func Value(): int32 { return 1; }\n");
  scalar.compile();
  test.expect(scalar.llvm.has_value() &&
                  !scalar.contains("call void @cloth_rt_gc_push_frame("),
              "reference-free callable emitted an empty GC root frame");

  CompiledSources unreachable;
  unreachable.add("Dead.co",
                  "static func Maybe(): Dead? {\n"
                  "  return null;\n"
                  "  return null;\n"
                  "}\n");
  unreachable.compile();
  test.expect(
      unreachable.llvm.has_value() &&
          count_occurrences(unreachable.llvm->text,
                            "call void @cloth_rt_gc_push_frame(") == 1 &&
          count_occurrences(unreachable.llvm->text,
                            "call void @cloth_rt_gc_pop_frame(") == 1,
      "unreachable return emitted an unbalanced GC frame pop");

  CompiledSources liveness;
  liveness.add("Lifetime.co",
               "static func Observe(Lifetime value) {\n"
               "  println(value);\n"
               "}\n");
  liveness.compile();
  test.expect(liveness.llvm.has_value(), "GC liveness module failed to emit");
  test.expect(liveness.contains("store ptr %v0, ptr %gc.v0, align 8\n"
                                "  store ptr null, ptr %s"),
              "last-use liveness did not clear a symbol root");
  test.expect(liveness.contains("call void @cloth_rt_print_object(ptr %v0)") &&
                  count_occurrences(liveness.llvm->text,
                                    "store ptr null, ptr %gc.v0") == 2,
              "last-use liveness did not clear a temporary root");

  CompiledSources branch_liveness;
  branch_liveness.add("BranchLifetime.co",
                      "static func Observe(BranchLifetime value, bool keep) {\n"
                      "  if (keep) { println(value); }\n"
                      "}\n");
  branch_liveness.compile();
  test.expect(branch_liveness.llvm.has_value() &&
                  count_occurrences(branch_liveness.llvm->text,
                                    "store ptr null, ptr %s") >= 3,
              "control-flow liveness did not clear a dead-path symbol root");
}

void wasm32_module(TestContext& test) {
  CompiledSources sources{cloth::TargetDataLayout::llvm_wasm32()};
  sources.add("Small.co",
              "int32 Value;\n"
              "Small() {}\n"
              "func Count(string[] values): int32 {\n"
              "  int32 count = 0;\n"
              "  for (var value in values) { count = count + 1; }\n"
              "  return count;\n"
              "}\n"
              "func Show(Small value): void {\n"
              "  println(value); println(1.5); return;\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "wasm32 module failed to emit");
  test.expect(sources.contains("target triple = \"wasm32-unknown-unknown\""),
              "wasm32 triple is missing");
  test.expect(
      sources.contains("@.cloth.type.0 = private constant { i64, ptr, i64, "
                       "i64, i64, ptr, i64 } { i64 0, ptr @.cloth.str.0, "
                       "i64 5, i64 12, i64 4, ptr null, i64 0 }") &&
          sources.contains("call ptr @cloth_rt_alloc(ptr @.cloth.type.0)"),
      "wasm32 object descriptor did not preserve its ABI layout");
  test.expect(sources.contains(" = phi i32 ") &&
                  sources.contains("call i32 @cloth_rt_array_length(ptr ") &&
                  sources.contains("call ptr @cloth_rt_array_element(ptr "),
              "wasm32 for iteration lost its portable array lowering");
  test.expect(sources.contains("call void @cloth_rt_print_object(ptr ") &&
                  sources.contains("call void @cloth_rt_print_f64(double ") &&
                  sources.contains("call void @cloth_rt_print_newline()"),
              "wasm32 typed println lowering is incomplete");
}

void print_and_native_entry_point(TestContext& test) {
  CompiledSources sources;
  sources.add("HelloWorld.co",
              "HelloWorld() {}\n"
              "static func Main(): void {\n"
              "  print(\"Hello, World!\\n\");\n"
              "  print(7);\n"
              "  print(true);\n"
              "  print('C');\n"
              "  print(1.5);\n"
              "  HelloWorld value = HelloWorld();\n"
              "  println(value);\n"
              "  println(null);\n"
              "  println();\n"
              "}\n");
  sources.compile(cloth::LlvmIrOptions{true});

  test.expect(sources.llvm.has_value(), "native entry module failed to emit");
  test.expect(sources.contains("declare void @cloth_rt_print(ptr)"),
              "print runtime boundary is missing");
  test.expect(sources.contains("call void @cloth_rt_print(ptr %v0)"),
              "string print intrinsic was not lowered");
  test.expect(sources.contains("call void @cloth_rt_print_i32(i32 7)"),
              "int32 print intrinsic was not lowered");
  test.expect(sources.contains("zext i1 true to i8") &&
                  sources.contains("call void @cloth_rt_print_bool(i8 %addr"),
              "bool print intrinsic was not lowered with its ABI width");
  test.expect(sources.contains("call void @cloth_rt_print_char(i32 67)"),
              "char print intrinsic was not lowered");
  test.expect(sources.contains("call void @cloth_rt_print_f64(double "),
              "float64 print intrinsic was not lowered");
  test.expect(
      sources.contains("call void @cloth_rt_print_object(ptr ") &&
          sources.contains("call void @cloth_rt_print_object(ptr null)"),
      "object print intrinsic was not lowered");
  test.expect(sources.contains("call void @cloth_rt_print_newline()"),
              "println did not lower its line feed");
  test.expect(sources.contains("define void @_C1F10_HelloWorld4_MainP0()") &&
                  sources.contains("ret void") &&
                  sources.contains("define i32 @main()") &&
                  sources.contains("call void @_C1F10_HelloWorld4_MainP0()"),
              "explicit void Main or its native entry adapter was not emitted");

  CompiledSources exit_code;
  exit_code.add("Program.co", "static func Main(): int32 { return 7; }\n");
  exit_code.compile(cloth::LlvmIrOptions{true});
  test.expect(
      exit_code.contains("%exit_code = call i32 @_C1F7_Program4_MainP0()") &&
          exit_code.contains("ret i32 %exit_code"),
      "int32 Main did not supply the process exit code");
}

void rejects_invalid_native_entry_points(TestContext& test) {
  CompiledSources missing;
  missing.add("Library.co", "func Read(): int { return 1; }\n");
  missing.compile(cloth::LlvmIrOptions{true});
  test.expect(!missing.llvm &&
                  has_diagnostic(missing.diagnostics,
                                 "requires a public static 'Main' function"),
              "missing native entry point was accepted");

  CompiledSources invalid;
  invalid.add("Program.co",
              "static func Main(int value): int { return value; }\n");
  invalid.compile(cloth::LlvmIrOptions{true});
  test.expect(
      !invalid.llvm && has_diagnostic(invalid.diagnostics,
                                      "entry point 'Main' must be public"),
      "invalid native entry signature was accepted");

  CompiledSources duplicate;
  duplicate.add("First.co", "static func Main() {}\n");
  duplicate.add("Second.co", "static func Main(): int { return 0; }\n");
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
      {"arrays", arrays},
      {"strings", strings},
      {"call receivers", call_receivers},
      {"static members", static_members},
      {"null ergonomics", null_ergonomics},
      {"GC root frames", gc_root_frames},
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
