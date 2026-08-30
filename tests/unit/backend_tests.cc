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

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

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

void integer_binary_lowering(TestContext& test) {
  CompiledSources sources;
  sources.add(
      "Binary.co",
      "func Transform(int32 value, uint64 count, byte[] bytes): int32 {\n"
      "  int32 result = (value & 255) | (value ^ 7);\n"
      "  result = result >> count;\n"
      "  result::writeLittleEndian(bytes, 0);\n"
      "  return bytes::readInt32LittleEndian(0);\n"
      "}\n");
  sources.compile();
  test.expect(sources.result->is_valid && sources.llvm,
              "integer binary module failed LLVM lowering");
  test.expect(
      sources.contains(" = and i32 ") && sources.contains(" = or i32 ") &&
          sources.contains(" = xor i32 ") && sources.contains(" = ashr i32 "),
      "integer operators use the wrong LLVM instructions");
  test.expect(sources.contains("call void @cloth_rt_require_shift_count") &&
                  sources.contains("call void @cloth_rt_integer_write") &&
                  sources.contains("call i64 @cloth_rt_integer_read"),
              "checked shift or endian runtime boundaries are missing");
}

void numeric_literal_and_widening_lowering(TestContext& test) {
  CompiledSources sources;
  sources.add("Numbers.co",
              "func Expand(int16 signedSmall, uint16 unsignedSmall, "
              "float32 single): float64 {\n"
              "  int32 signedWide = signedSmall;\n"
              "  int32 unsignedWide = unsignedSmall;\n"
              "  int64 literal = 10;\n"
              "  literal = 20;\n"
              "  uint64 maximum = 18446744073709551615;\n"
              "  float ratio = 0.5;\n"
              "  signedWide += signedSmall;\n"
              "  return single;\n"
              "}\n");
  sources.compile();

  test.expect(sources.result->is_valid,
              "numeric literal or widening program failed compilation");
  test.expect(sources.llvm.has_value(),
              "numeric literal or widening program emitted no LLVM IR");
  test.expect(sources.contains("sext i16") && sources.contains("zext i16") &&
                  sources.contains("fpext float"),
              "LLVM numeric widening uses the wrong extension operations");
  test.expect(sources.contains("i64 18446744073709551615"),
              "uint64 literal lost its full unsigned range");
  test.expect(sources.contains("store i64 20"),
              "contextual int64 assignment was not lowered at its target type");
  test.expect(sources.contains("float 5.000000000e-01"),
              "contextual float32 literal was not rounded and lowered once");
}

void checked_numeric_conversion_lowering(TestContext& test) {
  CompiledSources sources;
  sources.add("Conversions.co",
              "func Narrow(int32 value): int8 { return int8(value); }\n"
              "func Unsigned(int32 value): uint32 { return uint(value); }\n"
              "func Whole(float64 value): int32 { return int32(value); }\n"
              "func Decimal(int32 value): float32 { return float(value); }\n"
              "func Single(float64 value): float32 { return float(value); }\n"
              "func Constant(): int32 { return int32(12.9); }\n");
  sources.compile();

  test.expect(sources.result->is_valid,
              "checked numeric conversion program failed compilation");
  test.expect(sources.llvm.has_value(),
              "checked numeric conversion program emitted no LLVM IR");
  test.expect(
      sources.contains(
          "declare void @cloth_rt_require_numeric_conversion(i8)") &&
          sources.contains("call void @cloth_rt_require_numeric_conversion"),
      "checked numeric conversion runtime boundary is missing");
  test.expect(sources.contains(" = trunc i32 ") &&
                  sources.contains(" = fptosi double ") &&
                  sources.contains(" = sitofp i32 ") &&
                  sources.contains(" = fptrunc double "),
              "LLVM numeric conversion uses the wrong operations");
  test.expect(sources.contains("call double @llvm.trunc.f64(double ") &&
                  sources.contains("fcmp oge double") &&
                  sources.contains("fcmp olt double"),
              "floating-to-integer conversion lacks checked truncation");
  test.expect(sources.contains("ret i32 12"),
              "constant floating-to-integer conversion was not folded");
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
          sources.contains("@.cloth.type.virtuals.0 = private constant [1 x "
                           "ptr] [ptr @_C1F5_Model3_GetP0]") &&
          sources.contains("@.cloth.type.0 = private constant { i64, ptr, "
                           "ptr, i64, i64, i64, ptr, i64, ptr, i64, ptr, "
                           "i64 } { i64 "
                           "0, ptr "
                           "null, ptr @.cloth.str.0, i64 5, i64 32, i64 8, ptr "
                           "@.cloth.type.references.0, i64 1, ptr "
                           "@.cloth.type.virtuals.0, i64 1, ptr null, i64 0 }"),
      "file-class type descriptor metadata is missing");
  test.expect(sources.contains("define internal ptr @_C1I5_Model4_Name") &&
                  sources.contains("call ptr @_C1I5_Model4_Name(ptr %self)"),
              "field initializer was not composed with construction");
  test.expect(sources.contains("call ptr @cloth_rt_alloc(ptr @.cloth.type.0)"),
              "constructor does not allocate through its type descriptor");
  test.expect(sources.contains("getelementptr i8, ptr %self, i64 16"),
              "field access does not use its verified ABI offset");
}

void inherited_type_descriptors(TestContext& test) {
  CompiledSources sources;
  sources.add("Derived.co", "class : Base {\nint32 Count;\nBase? Owner;\n}\n");
  sources.add("Base.co", "class {\nstring? Name;\nbyte Tag;\n}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(), "derived class module failed to emit");
  test.expect(
      sources.contains("@.cloth.type.references.0 = private unnamed_addr "
                       "constant [2 x i64] [i64 16, i64 40]") &&
          sources.contains(
              "@.cloth.type.0 = private constant { i64, ptr, ptr, i64, i64, "
              "i64, ptr, i64, ptr, i64, ptr, i64 } { i64 0, ptr "
              "@.cloth.type.1, ptr "
              "@.cloth.str.0, i64 7, i64 48, i64 8, ptr "
              "@.cloth.type.references.0, i64 2, ptr null, i64 0, ptr null, "
              "i64 0 }"),
      "derived descriptor lost its parent pointer or inherited GC map");
  test.expect(sources.contains(
                  "@.cloth.type.1 = private constant { i64, ptr, ptr, i64, "
                  "i64, i64, ptr, i64, ptr, i64, ptr, i64 } { i64 0, ptr "
                  "null, ptr "),
              "root descriptor unexpectedly gained a parent pointer");
}

void constructor_chaining(TestContext& test) {
  CompiledSources sources;
  sources.add("Base.co", "Base(int32 value) { println(value); }\n");
  sources.add("Derived.co",
              "class : Base {\n"
              "  Derived(int32 value): Base(value) { println(value + 1); }\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "constructor chain failed LLVM lowering");
  test.expect(
      sources.contains(
          "define internal void @_C1I4_Base4_BaseP1_i32(ptr %self, i32 ") &&
          sources.contains(
              "define internal void @_C1I7_Derived7_DerivedP1_i32(ptr %self, "
              "i32 "),
      "constructor initializer entry points were not emitted");
  test.expect(
      count_occurrences(sources.llvm->text,
                        "call void @_C1I4_Base4_BaseP1_i32(ptr %self, i32 ") ==
          2,
      "derived allocation and initializer entries did not chain to the base");
  test.expect(
      count_occurrences(sources.llvm->text,
                        "call ptr @cloth_rt_alloc(ptr @.cloth.type.1)") == 1,
      "derived constructor did not allocate exactly one object");
}

void inherited_member_access_and_subtyping(TestContext& test) {
  CompiledSources sources;
  sources.add("Base.co",
              "int32 Value;\n"
              "Base(int32 value) { Value = value; }\n"
              "func Read(): int32 { return Value; }\n"
              "static func Kind(): int32 { return 1; }\n");
  sources.add("Derived.co",
              "class : Base { Derived(int32 value): Base(value) {} }\n");
  sources.add("Use.co",
              "func Upcast(Derived value): Base { return value; }\n"
              "func Read(Derived value): int32 {\n"
              "  value.Value = 2;\n"
              "  return value.Read() + Derived.Kind();\n"
              "}\n"
              "func Check(Base value): bool { return value is Base; }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "inherited behavior failed LLVM lowering");
  test.expect(sources.contains("@.cloth.type.virtuals.0 = private constant "
                               "[1 x ptr] [ptr @_C1F4_Base4_ReadP0]") &&
                  sources.contains(" = getelementptr inbounds ptr, ptr ") &&
                  sources.contains("call i32 @_C1F4_Base4_KindP0()"),
              "inherited virtual or static calls lost their ABI targets");
  test.expect(sources.contains("getelementptr i8, ptr ") &&
                  sources.contains(", i64 16\n"),
              "inherited field access lost its base offset");
  test.expect(sources.contains("call i8 @cloth_rt_object_is_type(ptr ") &&
                  sources.contains(", ptr @.cloth.type.0)"),
              "base type test lost its ancestry-aware runtime boundary");
}

void virtual_dispatch(TestContext& test) {
  CompiledSources sources;
  sources.add("Base.co",
              "func Value(): int32 { return 1; }\n"
              "func Name(): string { return \"base\"; }\n");
  sources.add("Derived.co",
              "class : Base {\n"
              "  override func Value(): int32 { return 2; }\n"
              "  func Extra(): int32 { return 3; }\n"
              "}\n");
  sources.add("Use.co",
              "func Read(Base value): int32 { return value.Value(); }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "virtual dispatch module failed LLVM emission");
  test.expect(
      sources.contains(
          "@.cloth.type.virtuals.0 = private constant [2 x ptr] [ptr "
          "@_C1F4_Base5_ValueP0, ptr @_C1F4_Base4_NameP0]") &&
          sources.contains(
              "@.cloth.type.virtuals.1 = private constant [3 x ptr] [ptr "
              "@_C1F7_Derived5_ValueP0, ptr @_C1F4_Base4_NameP0, ptr "
              "@_C1F7_Derived5_ExtraP0]"),
      "derived vtable did not replace and extend stable base slots");
  test.expect(sources.contains(
                  "getelementptr inbounds { i64, ptr, ptr, i64, i64, i64, ptr, "
                  "i64, ptr, i64, ptr, i64 }, ptr ") &&
                  sources.contains("getelementptr inbounds ptr, ptr ") &&
                  sources.contains("call i32 %"),
              "virtual call was not emitted through descriptor slot zero");
  test.expect(!sources.contains("call i32 @_C1F4_Base5_ValueP0(ptr %receiver"),
              "base-typed call bypassed dynamic dispatch");
}

void base_qualified_call(TestContext& test) {
  CompiledSources sources;
  sources.add("Base.co", "func Value(): int32 { return 1; }\n");
  sources.add("Derived.co",
              "class : Base {\n"
              "  override func Value(): int32 {\n"
              "    return super.Value() + 1;\n"
              "  }\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "base-qualified call failed LLVM emission");
  test.expect(sources.contains("call i32 @_C1F4_Base5_ValueP0(ptr %receiver)"),
              "base-qualified call did not use the selected base ABI symbol");
}

void abstract_function_stub(TestContext& test) {
  CompiledSources sources;
  sources.add("Shape.co", "abstract class { abstract func Area(): int32; }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "abstract declaration module failed LLVM emission");
  test.expect(sources.result->is_valid,
              "abstract declaration compilation was marked invalid");
  if (!sources.llvm || sources.result->abi.files.empty() ||
      sources.result->abi.files[0].functions.empty()) {
    return;
  }
  const std::string& name =
      sources.result->abi.files[0].functions[0].mangled_name;
  const std::size_t name_position = sources.llvm->text.find("@" + name + "(");
  const std::size_t body_end = sources.llvm->text.find("\n}", name_position);
  test.expect(
      name_position != std::string::npos && body_end != std::string::npos,
      "abstract function ABI definition was not emitted");
  if (name_position != std::string::npos && body_end != std::string::npos) {
    const std::string_view body{sources.llvm->text.data() + name_position,
                                body_end - name_position};
    test.expect(body.find("unreachable") != std::string_view::npos,
                "abstract function ABI body was not an unreachable stub");
    test.expect(body.find("ret i32") == std::string_view::npos,
                "abstract function synthesized a value implementation");
  }
}

void interface_dispatch(TestContext& test) {
  CompiledSources sources;
  sources.add("Renderable.co",
              "interface { func Render(int32 width): string; }\n");
  sources.add("Widget.co",
              "class is Renderable {\n"
              "  func Render(int32 width): string { return \"widget\"; }\n"
              "}\n");
  sources.add(
      "Use.co",
      "func Read(Renderable value): string {\n"
      "  return value.Render(1);\n"
      "}\n"
      "func Check(object value): bool { return value is Renderable; }\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "interface dispatch module failed LLVM emission");
  if (!sources.result || sources.result->abi.files.size() < 2) {
    return;
  }
  const auto interface_id =
      sources.result->semantics.file({0}).interface_id.value_or(0);
  const cloth::AbiTypeDescriptor& widget =
      sources.result->abi.files[1].type_descriptor;
  test.expect(widget.interfaces.size() == 1 &&
                  widget.interfaces[0].interface_id == interface_id &&
                  widget.interfaces[0].functions.size() == 1,
              "class descriptor lost its interface dispatch map");
  test.expect(
      sources.contains("@.cloth.type.interface.functions.1.0 = private "
                       "constant [1 x ptr] [ptr "
                       "@_C1F6_Widget6_RenderP1_i32]") &&
          sources.contains("@.cloth.type.interfaces.1 = private constant "
                           "[1 x { i64, ptr, i64 }]") &&
          sources.contains("call ptr @cloth_rt_interface_function(ptr ") &&
          sources.contains(", i64 " + std::to_string(interface_id) +
                           ", i64 0)") &&
          sources.contains("call i8 @cloth_rt_object_is_interface(ptr "),
      "interface calls or checked operations lost runtime dispatch metadata");
  test.expect(!sources.contains("@.cloth.type.0 = private constant"),
              "interface declaration emitted an allocatable class descriptor");
}

void arrays(TestContext& test) {
  CompiledSources sources;
  sources.add("Arrays.co",
              "func Sum(): int32 {\n"
              "  int32[] values = [1, 2, 3];\n"
              "  string[] labels = [\"cloth\"];\n"
              "  values[1] = 4;\n"
              "  return values::length + values[0];\n"
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
              "  println(joined::isEmpty);\n"
              "  return joined::length + joined::byteLength;\n"
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
      "string meta queries did not use their runtime boundaries");
  test.expect(sources.contains("icmp ne i8 "),
              "runtime string booleans were not converted to LLVM i1");
}

void object_model(TestContext& test) {
  CompiledSources sources;
  sources.add("Objects.co",
              "Objects() {}\n"
              "static func Main() {\n"
              "  object value = Objects();\n"
              "  bool exact = value is Objects;\n"
              "  bool stringLike = value is string;\n"
              "  Objects? cast = value as Objects?;\n"
              "  string name = value::typeName;\n"
              "}\n");
  sources.compile();

  test.expect(sources.llvm.has_value(),
              "object operations failed LLVM emission");
  test.expect(sources.contains("call ptr @cloth_rt_object_type_name(ptr "),
              "object typeName did not use its runtime boundary");
  test.expect(sources.contains("call i8 @cloth_rt_object_is_type(ptr ") &&
                  sources.contains("call i8 @cloth_rt_object_is_kind(ptr "),
              "checked object operations lost runtime type checks");
  test.expect(sources.contains(" = select i1 "),
              "checked cast did not select the value or null");
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
  test.expect(
      sources.contains("call void @cloth_rt_require_receiver(ptr %receiver)"),
      "unqualified call did not forward its receiver");
  test.expect(sources.contains("call void @cloth_rt_require_receiver(ptr %v0)"),
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
                                "call void @cloth_rt_gc_push_frame(") == 4 &&
                  count_occurrences(sources.llvm->text,
                                    "call void @cloth_rt_gc_pop_frame(") == 5,
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
      sources.contains("@.cloth.type.0 = private constant { i64, ptr, ptr, "
                       "i64, i64, i64, ptr, i64, ptr, i64, ptr, i64 } { i64 "
                       "0, ptr "
                       "null, ptr "
                       "@.cloth.str.0, i64 5, i64 12, i64 4, ptr null, "
                       "i64 0, ptr @.cloth.type.virtuals.0, i64 2, ptr null, "
                       "i64 0 }") &&
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
      has_diagnostic(sources.diagnostics, "is out of range for 'int32'"),
      "out-of-range literal produced the wrong diagnostic");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"target header", target_header},
      {"arithmetic and control flow", arithmetic_and_control_flow},
      {"integer binary lowering", integer_binary_lowering},
      {"numeric literal and widening lowering",
       numeric_literal_and_widening_lowering},
      {"checked numeric conversion lowering",
       checked_numeric_conversion_lowering},
      {"object construction", object_construction},
      {"inherited type descriptors", inherited_type_descriptors},
      {"constructor chaining", constructor_chaining},
      {"inherited member access and subtyping",
       inherited_member_access_and_subtyping},
      {"virtual dispatch", virtual_dispatch},
      {"base-qualified call", base_qualified_call},
      {"abstract function stub", abstract_function_stub},
      {"interface dispatch", interface_dispatch},
      {"arrays", arrays},
      {"strings", strings},
      {"object model", object_model},
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

  return cloth::test::run_tests(tests);
}
