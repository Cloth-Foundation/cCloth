#include "cloth/abi/abi.h"
#include "cloth/abi/abi_verifier.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/sema/semantic_model.h"
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

struct CompiledSource {
  explicit CompiledSource(
      std::string text,
      cloth::TargetDataLayout target = cloth::TargetDataLayout::llvm_x86_64())
      : compilation(std::move(target)) {
    compilation.add_source(cloth::SourceFile::from_memory(
        std::filesystem::path{"Layout.co"}, std::move(text)));
    result.emplace(compilation.analyze(diagnostics));
  }

  cloth::Compilation compilation;
  cloth::DiagnosticEngine diagnostics;
  std::optional<cloth::CompilationResult> result;
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

void x86_64_type_layout(TestContext& test) {
  const CompiledSource source{"byte Small;\nint64 Wide;\nstring? Name;\n"};
  const cloth::CompilationResult& result = *source.result;
  const cloth::AbiModule& abi = result.abi;
  const cloth::TypeId bool_type = *result.semantics.find_type("bool");
  const cloth::TypeId int64_type = *result.semantics.find_type("int64");

  test.expect(result.is_valid, "valid x86-64 ABI failed verification");
  test.expect(abi.target.pointer == cloth::SizeAlignment{8, 8},
              "x86-64 pointer layout is wrong");
  test.expect(
      abi.types[bool_type.value].bit_width == 1 &&
          abi.types[bool_type.value].storage == cloth::SizeAlignment{1, 1},
      "bool register and storage layout is wrong");
  test.expect(abi.types[int64_type.value].storage == cloth::SizeAlignment{8, 8},
              "int64 layout is wrong");
}

void class_field_layout(TestContext& test) {
  const CompiledSource source{"byte Small;\nint64 Wide;\nstring? Name;\n"};
  const cloth::AbiFileClass& file = source.result->abi.files[0];
  const cloth::AbiClassLayout& layout = file.layout;

  test.expect(layout.header_size == 16, "object header is not two words");
  test.expect(layout.size == 40 && layout.alignment == 8,
              "x86-64 class size or alignment is wrong");
  test.expect(layout.fields.size() == 3 && layout.fields[0].offset == 16 &&
                  layout.fields[1].offset == 24 &&
                  layout.fields[2].offset == 32,
              "fields were not laid out in declaration order");
  test.expect(
      file.type_descriptor.kind == cloth::AbiHeapObjectKind::kFileClass &&
          file.type_descriptor.name == "Layout" &&
          file.type_descriptor.size == layout.size &&
          file.type_descriptor.alignment == layout.alignment &&
          file.type_descriptor.reference_offsets ==
              std::vector<std::uint64_t>{32},
      "file-class descriptor lost its identity or reference layout");
}

void wasm32_layout(TestContext& test) {
  const CompiledSource source{"byte Small;\nint64 Wide;\nstring? Name;\n",
                              cloth::TargetDataLayout::llvm_wasm32()};
  const cloth::AbiModule& abi = source.result->abi;
  const cloth::AbiClassLayout& layout = abi.files[0].layout;

  test.expect(source.result->is_valid, "valid wasm32 ABI failed verification");
  test.expect(abi.target.pointer == cloth::SizeAlignment{4, 4},
              "wasm32 pointer layout is wrong");
  test.expect(layout.header_size == 8 && layout.size == 32 &&
                  layout.fields[0].offset == 8 &&
                  layout.fields[1].offset == 16 &&
                  layout.fields[2].offset == 24,
              "wasm32 class layout is wrong");
  test.expect(abi.files[0].type_descriptor.reference_offsets ==
                  std::vector<std::uint64_t>{24},
              "wasm32 descriptor has the wrong reference offset");
}

void callable_abi(TestContext& test) {
  const CompiledSource source{
      "Layout(int32 value) {}\n"
      "func Build(int32 value): Layout { return Layout(value); }\n"
      "func hidden(): bool { return true; }\n"};
  const cloth::AbiFileClass& file = source.result->abi.files[0];
  const cloth::AbiCallable& build = file.functions[0];
  const cloth::AbiCallable& hidden = file.functions[1];
  const cloth::AbiCallable& constructor = file.constructors[0];

  test.expect(build.linkage == cloth::AbiLinkage::kExternal &&
                  hidden.linkage == cloth::AbiLinkage::kInternal,
              "capitalization did not determine ABI linkage");
  test.expect(build.calling_convention == cloth::AbiCallingConvention::kC,
              "function ABI does not use the C calling convention");
  test.expect(
      build.parameters.size() == 2 &&
          build.parameters[0].kind == cloth::AbiParameterKind::kReceiver &&
          build.parameters[1].kind == cloth::AbiParameterKind::kExplicit,
      "function ABI does not contain its uniform receiver slot");
  test.expect(constructor.parameters.size() == 1 &&
                  constructor.parameters[0].kind ==
                      cloth::AbiParameterKind::kExplicit &&
                  constructor.return_type ==
                      source.result->semantics.file(cloth::FileId{0}).type,
              "constructor ABI does not return the allocated object");
}

void static_member_abi(TestContext& test) {
  const CompiledSource source{
      "static final int32 Version = 12;\n"
      "int32 Value;\n"
      "static func Read(): int32 { return Version; }\n"};
  const cloth::AbiFileClass& file = source.result->abi.files[0];

  test.expect(source.result->is_valid,
              "valid static members failed ABI lowering");
  test.expect(file.layout.fields.size() == 1 &&
                  file.layout.fields[0].symbol ==
                      source.result->semantics.file(cloth::FileId{0}).fields[1],
              "static field leaked into instance layout");
  test.expect(file.static_fields.size() == 1 &&
                  file.static_fields[0].mangled_name == "_C1S6_Layout7_Version",
              "static field ABI is missing or unstable");
  test.expect(file.functions[0].parameters.empty(),
              "static function gained a receiver parameter");
}

void void_abi(TestContext& test) {
  const CompiledSource source{
      "func Explicit(): void { return; }\n"
      "func Implicit() {}\n"};
  const cloth::SemanticModel& semantics = source.result->semantics;
  const cloth::TypeId void_type = semantics.void_type();
  const cloth::AbiTypeLayout& layout =
      source.result->abi.types[void_type.value];
  const std::vector<cloth::AbiCallable>& functions =
      source.result->abi.files[0].functions;

  test.expect(source.result->is_valid, "valid void ABI failed verification");
  test.expect(layout.kind == cloth::AbiTypeKind::kVoid &&
                  layout.storage == cloth::SizeAlignment{0, 1},
              "void has an invalid ABI layout");
  test.expect(functions.size() == 2 && functions[0].return_type == void_type &&
                  functions[1].return_type == void_type,
              "explicit and implicit void functions have different ABIs");
}

void deterministic_mangling(TestContext& test) {
  const CompiledSource source{
      "func Pick(int value): int { return value; }\n"
      "func Pick(bool value): bool { return value; }\n"};
  const std::vector<cloth::AbiCallable>& functions =
      source.result->abi.files[0].functions;

  test.expect(functions[0].mangled_name == "_C1F6_Layout4_PickP1_i32",
              "int32 overload has an unstable mangled name");
  test.expect(functions[1].mangled_name == "_C1F6_Layout4_PickP1_b",
              "bool overload has an unstable mangled name");
  test.expect(functions[0].mangled_name != functions[1].mangled_name,
              "overloads have colliding mangled names");
}

void array_abi(TestContext& test) {
  const CompiledSource source{
      "func First(int32[] values): int32 { return values[0]; }\n"};
  const cloth::SemanticModel& semantics = source.result->semantics;
  const cloth::AbiCallable& callable = source.result->abi.files[0].functions[0];
  const cloth::TypeId array_type =
      semantics.symbol(callable.symbol).parameter_types[0];
  const cloth::AbiTypeLayout& layout =
      source.result->abi.types[array_type.value];

  test.expect(source.result->is_valid, "valid array ABI failed verification");
  test.expect(layout.kind == cloth::AbiTypeKind::kReference &&
                  layout.storage == cloth::SizeAlignment{8, 8},
              "array ABI is not an opaque reference");
  test.expect(callable.mangled_name.find("_ai32") != std::string::npos,
              "array parameter has no structural type encoding");
}

void nullable_abi(TestContext& test) {
  const CompiledSource source{
      "func Maybe(Layout? value): Layout? { return value; }\n"};
  const cloth::SemanticModel& semantics = source.result->semantics;
  const cloth::AbiCallable& callable = source.result->abi.files[0].functions[0];
  const cloth::TypeId nullable =
      semantics.symbol(callable.symbol).parameter_types[0];
  const cloth::SemanticType& semantic_type = semantics.type(nullable);
  const cloth::AbiTypeLayout& layout = source.result->abi.types[nullable.value];

  test.expect(source.result->is_valid,
              "valid nullable ABI failed verification");
  test.expect(
      semantic_type.kind == cloth::TypeKind::kNullable &&
          semantic_type.element_type == semantics.file(cloth::FileId{0}).type,
      "nullable ABI type lost its semantic identity");
  test.expect(layout.kind == cloth::AbiTypeKind::kReference &&
                  layout.storage == cloth::SizeAlignment{8, 8},
              "nullable type does not use reference layout");
  test.expect(callable.mangled_name == "_C1F6_Layout5_MaybeP1_r6_Layout",
              "nullable ABI mangling did not erase the source qualifier");
}

void package_qualified_mangling(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_source(
      cloth::SourceFile::from_memory("left/User.co",
                                     "func Make(): int { return 1; }\n"),
      "left");
  compilation.add_source(
      cloth::SourceFile::from_memory("right/User.co",
                                     "func Make(): int { return 2; }\n"),
      "right");
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);

  test.expect(result.is_valid,
              "equal class stems in different packages were rejected");
  const std::string& left = result.abi.files[0].functions[0].mangled_name;
  const std::string& right = result.abi.files[1].functions[0].mangled_name;
  test.expect(left != right,
              "package-qualified callables have colliding names");
  test.expect(left.find("left.User") != std::string::npos &&
                  right.find("right.User") != std::string::npos,
              "mangled names do not retain qualified class identity");
}

void verifier_rejects_layout_corruption(TestContext& test) {
  const CompiledSource source{"int64 Wide;\n"};
  cloth::AbiModule broken = source.result->abi;
  broken.files[0].layout.fields[0].offset = 17;
  cloth::DiagnosticEngine diagnostics;

  test.expect(!cloth::verify_abi(broken, source.result->mir,
                                 source.result->semantics, diagnostics),
              "ABI verifier accepted a misaligned field");
  test.expect(has_diagnostic(diagnostics, "class layout does not match"),
              "ABI verifier reported the wrong layout invariant");

  broken = source.result->abi;
  broken.files[0].type_descriptor.reference_offsets = {17};
  cloth::DiagnosticEngine descriptor_diagnostics;
  test.expect(
      !cloth::verify_abi(broken, source.result->mir, source.result->semantics,
                         descriptor_diagnostics),
      "ABI verifier accepted invalid descriptor reference metadata");
  test.expect(
      has_diagnostic(descriptor_diagnostics,
                     "type descriptor does not match") ||
          has_diagnostic(descriptor_diagnostics, "invalid reference offset"),
      "invalid descriptor produced the wrong diagnostic");

  broken = source.result->abi;
  const cloth::TypeId int64_type = *source.result->semantics.find_type("int64");
  broken.types[int64_type.value].storage.alignment = 0;
  cloth::DiagnosticEngine type_diagnostics;
  test.expect(!cloth::verify_abi(broken, source.result->mir,
                                 source.result->semantics, type_diagnostics),
              "ABI verifier accepted a zero type alignment");
}

void verifier_rejects_invalid_target(TestContext& test) {
  const CompiledSource source{""};
  cloth::AbiModule broken = source.result->abi;
  broken.target.pointer.alignment = 3;
  cloth::DiagnosticEngine diagnostics;

  test.expect(!cloth::verify_abi(broken, source.result->mir,
                                 source.result->semantics, diagnostics),
              "ABI verifier accepted a non-power-of-two alignment");
  test.expect(has_diagnostic(diagnostics, "target data layout is invalid"),
              "invalid target produced the wrong diagnostic");
}

using TestFunction = void (*)(TestContext&);

struct TestCase {
  std::string_view name;
  TestFunction function;
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"x86-64 type layout", x86_64_type_layout},
      {"class field layout", class_field_layout},
      {"wasm32 layout", wasm32_layout},
      {"callable ABI", callable_abi},
      {"static member ABI", static_member_abi},
      {"void ABI", void_abi},
      {"deterministic mangling", deterministic_mangling},
      {"array ABI", array_abi},
      {"nullable ABI", nullable_abi},
      {"package-qualified mangling", package_qualified_mangling},
      {"verifier rejects layout corruption",
       verifier_rejects_layout_corruption},
      {"verifier rejects invalid target", verifier_rejects_invalid_target},
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
