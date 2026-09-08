// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM
// Exceptions. See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/diagnostics/diagnostic_engine.h"
#include "cloth/hir/hir.h"
#include "cloth/hir/hir_verifier.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/sema/semantic_model.h"
#include "cloth/source/source_file.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

void add(cloth::Compilation& compilation, std::filesystem::path path,
         std::string source) {
  compilation.add_source(
      cloth::SourceFile::from_memory(std::move(path), std::move(source)));
}

bool has_diagnostic(const cloth::DiagnosticEngine& diagnostics,
                    std::string_view text) {
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string result;
  for (const cloth::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    result += diagnostic.message + '\n';
  }
  return result;
}

void add_value_types(cloth::Compilation& compilation) {
  add(compilation, "Status.co", "enum { Ready, Waiting }\n");
  add(compilation, "Point.co", R"(
    struct {
      int32 X;
      int32? Previous;
      Point(int32 x) { X = x; Previous = null; }
      func Value(): int32 { return X; }
    }
  )");
}

void nullable_value_surface(TestContext& test) {
  cloth::Compilation compilation;
  add_value_types(compilation);
  add(compilation, "Values.co", R"(
    bool? BoolValue;
    char? CharValue;
    byte? ByteValue;
    int8? Int8Value;
    int16? Int16Value;
    int32? Count;
    int64? Int64Value;
    uint8? Uint8Value;
    uint16? Uint16Value;
    uint32? Uint32Value;
    uint64? Uint64Value;
    float32? Float32Value;
    float64? Float64Value;
    func Convert(int16? small, Status? state, Point? point,
                 bool? enabled): int64? {
      int32? present = 1;
      int32? absent;
      int32?[] explicit = [1, null];
      var inferred = [1, null];
      int64? widened = small;
      Status? ready = Status.Ready;
      Point? copy = point;
      Point? origin = Point(0);
      int? intAlias = null;
      uint? uintAlias = null;
      float? floatAlias = null;
      int32? joined;
      if (enabled) { joined = 2; } else { joined = null; }
      if (enabled) { bool payload = enabled; }
      if (present != null) { int32 payload = present; }
      bool same = present == absent;
      bool reversed = null != present;
      bool stateMatches = state == Status.Ready;
      bool pointMatches = point == origin;
      return widened;
    }
    func Required(int32? value): int32 { return value ?? 0; }
    func Asserted(int32? value): int32 { return value!; }
  )");

  cloth::DiagnosticEngine diagnostics;
  const cloth::FrontendResult frontend =
      compilation.analyze_frontend(diagnostics);
  test.expect(frontend.is_valid && !diagnostics.has_errors(),
              "uniform nullable value source failed frontend analysis:\n" +
                  messages(diagnostics));
  if (!frontend.is_valid) {
    return;
  }

  std::set<cloth::TypeKind> nullable_primitives;
  bool found_nullable_enum = false;
  bool found_nullable_struct = false;
  bool found_nullable_elements = false;
  for (const cloth::SemanticType& type : frontend.semantics.types()) {
    if (type.kind == cloth::TypeKind::kNullable && type.element_type) {
      const cloth::TypeKind underlying =
          frontend.semantics.type(*type.element_type).kind;
      if (underlying >= cloth::TypeKind::kBool &&
          underlying <= cloth::TypeKind::kFloat64) {
        nullable_primitives.insert(underlying);
      }
      found_nullable_enum =
          found_nullable_enum || underlying == cloth::TypeKind::kEnum;
      found_nullable_struct =
          found_nullable_struct || underlying == cloth::TypeKind::kStruct;
    }
    if (type.kind == cloth::TypeKind::kArray && type.element_type) {
      found_nullable_elements =
          found_nullable_elements ||
          frontend.semantics.type(*type.element_type).kind ==
              cloth::TypeKind::kNullable;
    }
  }
  test.expect(nullable_primitives.size() == 13 && found_nullable_enum &&
                  found_nullable_struct && found_nullable_elements,
              "semantic types lost a supported nullable value shape");

  const cloth::MirModule mir =
      cloth::lower_to_mir(frontend.hir, frontend.semantics);
  test.expect(cloth::verify_mir(mir, frontend.semantics, diagnostics),
              "nullable value MIR failed verification");
  bool found_default_absent = false;
  bool found_lift = false;
  bool found_equality = false;
  for (const cloth::MirFileClass& file : mir.files) {
    for (const cloth::MirCallable& function : file.functions) {
      for (const cloth::MirBasicBlock& block : function.body.blocks) {
        for (const cloth::MirInstruction& instruction : block.instructions) {
          if (const auto* declaration =
                  std::get_if<cloth::MirDeclareLocalInstruction>(
                      &instruction.data)) {
            found_default_absent =
                found_default_absent ||
                (frontend.semantics.symbol(declaration->symbol).name ==
                     "absent" &&
                 declaration->initializer.has_value());
          }
          if (const auto* conversion =
                  std::get_if<cloth::MirConvertInstruction>(
                      &instruction.data)) {
            found_lift =
                found_lift ||
                conversion->kind == cloth::MirConversionKind::kLiftNullable;
          }
          found_equality =
              found_equality ||
              std::holds_alternative<cloth::MirNullableEqualInstruction>(
                  instruction.data);
        }
      }
    }
  }
  test.expect(found_default_absent && found_lift && found_equality,
              "nullable value MIR lost absence, lifting, or equality");
}

void safe_operations(TestContext& test) {
  cloth::Compilation compilation;
  add_value_types(compilation);
  add(compilation, "User.co", R"(
    int32 Count;
    User(int32 count) { Count = count; }
    func Add(int32 value): int32 { return Count + value; }
    func Flush() {}
    static func Build(): User { return User(0); }
  )");
  add(compilation, "Safe.co", R"(
    func Next(): int32 { return 1; }
    func Inspect(User? user, Point? point, string? text, Worker? worker,
                 Reader? reader, Failure? failure, Base? base) throws Failure {
      int32? field = user?.Count;
      int32? called = user?.Add(Next());
      user?.Flush();
      int32? value = point?.Value();
      int32? length = text?::length;
      int32? bytes = text?::byteLength;
      bool? empty = text?::isEmpty;
      string? userType = user?::typeName;
      string? pointType = point?::typeName;
      int32? risky = worker?.Risk();
      int32? interfaceValue = reader?.Read();
      int32? errorValue = failure?.Code();
      int32? virtualValue = base?.Read();
    }
  )");
  add(compilation, "Failure.co", R"(
    error {
      Failure() {}
      func Code(): int32? { return 7; }
    }
  )");
  add(compilation, "Worker.co", R"(
    func Risk(): int32 throws Failure { throw Failure(); }
  )");
  add(compilation, "Reader.co", "interface { func Read(): int32?; }\n");
  add(compilation, "Base.co", R"(
    func Read(): int32? { return 9; }
  )");

  cloth::DiagnosticEngine diagnostics;
  const cloth::FrontendResult frontend =
      compilation.analyze_frontend(diagnostics);
  test.expect(frontend.is_valid && !diagnostics.has_errors(),
              "safe nullable operations failed frontend analysis");
  if (!frontend.is_valid) {
    return;
  }

  std::size_t safe_calls = 0;
  std::size_t safe_meta = 0;
  for (const cloth::HirExpression& expression :
       frontend.hir.storage.expressions()) {
    if (const auto* call =
            std::get_if<cloth::HirCallExpression>(&expression.data)) {
      safe_calls += call->is_safe ? 1U : 0U;
    }
    safe_meta +=
        std::holds_alternative<cloth::HirSafeMetaExpression>(expression.data)
            ? 1U
            : 0U;
  }
  test.expect(safe_calls == 7 && safe_meta == 5,
              "HIR lost safe call or safe meta identity");
  test.expect(
      !frontend.semantics
           .symbol(frontend.semantics.file(cloth::FileId{3}).functions[1])
           .thrown_types.empty(),
      "safe throwing call lost its declared effect");

  const cloth::MirModule mir =
      cloth::lower_to_mir(frontend.hir, frontend.semantics);
  test.expect(cloth::verify_mir(mir, frontend.semantics, diagnostics),
              "safe nullable MIR failed verification");
  const cloth::MirBody& body = mir.files[3].functions[1].body;
  std::optional<std::size_t> next_block;
  std::optional<std::size_t> add_block;
  std::size_t presence_tests = 0;
  std::size_t phis = 0;
  for (std::size_t block_index = 0; block_index < body.blocks.size();
       ++block_index) {
    for (const cloth::MirInstruction& instruction :
         body.blocks[block_index].instructions) {
      presence_tests += std::holds_alternative<cloth::MirIsNonNullInstruction>(
                            instruction.data)
                            ? 1U
                            : 0U;
      phis += std::holds_alternative<cloth::MirPhiInstruction>(instruction.data)
                  ? 1U
                  : 0U;
      const auto* call =
          std::get_if<cloth::MirCallInstruction>(&instruction.data);
      if (call == nullptr) {
        continue;
      }
      const std::string& name = frontend.semantics.symbol(call->callable).name;
      if (name == "Next") {
        next_block = block_index;
      } else if (name == "Add") {
        add_block = block_index;
      }
    }
  }
  test.expect(next_block && add_block && next_block == add_block &&
                  *next_block != body.entry.value,
              "safe call arguments were evaluated before receiver presence");
  test.expect(presence_tests >= 13 && phis == 12,
              "safe operations lost explicit presence control flow");

  cloth::HirModule broken_hir = frontend.hir;
  bool changed_hir = false;
  for (const cloth::HirExpression& stored : broken_hir.storage.expressions()) {
    auto& expression = const_cast<cloth::HirExpression&>(stored);
    if (auto* call = std::get_if<cloth::HirCallExpression>(&expression.data);
        call != nullptr && call->is_safe) {
      call->is_safe = false;
      changed_hir = true;
      break;
    }
  }
  cloth::DiagnosticEngine hir_diagnostics;
  test.expect(
      changed_hir &&
          !cloth::verify_hir(broken_hir, frontend.semantics, hir_diagnostics) &&
          has_diagnostic(hir_diagnostics,
                         "safe call has incompatible HIR metadata"),
      "HIR verifier accepted corrupted safe-call metadata");

  cloth::HirModule broken_meta = frontend.hir;
  bool changed_meta = false;
  for (const cloth::HirExpression& stored : broken_meta.storage.expressions()) {
    auto& expression = const_cast<cloth::HirExpression&>(stored);
    if (std::holds_alternative<cloth::HirSafeMetaExpression>(expression.data)) {
      expression.type = frontend.semantics.bool_type();
      changed_meta = true;
      break;
    }
  }
  cloth::DiagnosticEngine meta_diagnostics;
  test.expect(changed_meta &&
                  !cloth::verify_hir(broken_meta, frontend.semantics,
                                     meta_diagnostics) &&
                  has_diagnostic(meta_diagnostics,
                                 "safe meta query has incompatible HIR "
                                 "metadata"),
              "HIR verifier accepted corrupted safe-meta metadata");

  cloth::HirModule broken_member = frontend.hir;
  bool changed_member = false;
  for (const cloth::HirExpression& stored :
       broken_member.storage.expressions()) {
    auto& expression = const_cast<cloth::HirExpression&>(stored);
    const auto* member =
        std::get_if<cloth::HirSafeMemberExpression>(&expression.data);
    if (member != nullptr && member->member &&
        frontend.semantics.symbol(*member->member).kind ==
            cloth::SymbolKind::kField) {
      expression.type = frontend.semantics.bool_type();
      changed_member = true;
      break;
    }
  }
  cloth::DiagnosticEngine member_diagnostics;
  test.expect(changed_member &&
                  !cloth::verify_hir(broken_member, frontend.semantics,
                                     member_diagnostics) &&
                  has_diagnostic(member_diagnostics,
                                 "safe field access has incompatible HIR "
                                 "metadata"),
              "HIR verifier accepted corrupted safe-field metadata");
}

void invalid_contract(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "User.co", R"(
    User() {}
    static func Build(): User { return User(); }
  )");
  add(compilation, "Invalid.co", R"(
    func Pick(int32 value) {}
    func Pick(int32? value) {}
    func Broken(User? user, string? text, int32? value, int32[]? values) {
      void? impossible;
      var unknown = null;
      int32 sum = value + 1;
      int32? badMeta = value?::length;
      int32? nonNullableMeta = sum?::length;
      string? slice = text?::slice(0, 1);
      int32? indexed = values?[0];
      User? built = user?.Build();
      bool checked = value is object;
    }
  )");

  cloth::DiagnosticEngine diagnostics;
  const cloth::FrontendResult frontend =
      compilation.analyze_frontend(diagnostics);
  test.expect(!frontend.is_valid && diagnostics.has_errors(),
              "invalid nullable value contract was accepted");
  test.expect(
      has_diagnostic(diagnostics, "duplicate function signature") &&
          has_diagnostic(diagnostics, "'void' cannot be nullable") &&
          has_diagnostic(diagnostics, "from null") &&
          has_diagnostic(diagnostics, "operator 'plus' cannot be applied") &&
          has_diagnostic(diagnostics,
                         "safe meta access requires a nullable value") &&
          has_diagnostic(diagnostics,
                         "safe callable meta operation '?::slice'") &&
          has_diagnostic(diagnostics,
                         "safe calls require an instance function") &&
          has_diagnostic(diagnostics,
                         "checked type operations require a managed "
                         "reference"),
      "invalid nullable value source produced incomplete diagnostics");
  test.expect(!has_diagnostic(diagnostics, "internal"),
              "invalid nullable value source leaked an internal diagnostic");
}

void value_equality_and_flow(TestContext& test) {
  cloth::Compilation compilation;
  add_value_types(compilation);
  add(compilation, "Box.co",
      "string Value; Box(string value) { Value = value; }");
  add(compilation, "Matrix.co", R"(
    func Equal(bool? boolValue, char? charValue, byte? byteValue,
               int8? int8Value, int16? int16Value, int32? int32Value,
               int64? int64Value, uint8? uint8Value, uint16? uint16Value,
               uint32? uint32Value, uint64? uint64Value,
               float32? float32Value, float64? float64Value,
               Status? state, Point? point, string? text, Box? box,
               int32[]? values): bool {
      bool equalBool = boolValue == boolValue;
      bool equalChar = charValue == charValue;
      bool equalByte = byteValue == byteValue;
      bool equalInt8 = int8Value == int8Value;
      bool equalInt16 = int16Value == int16Value;
      bool equalInt32 = int32Value == int32Value;
      bool equalInt64 = int64Value == int64Value;
      bool equalUint8 = uint8Value == uint8Value;
      bool equalUint16 = uint16Value == uint16Value;
      bool equalUint32 = uint32Value == uint32Value;
      bool equalUint64 = uint64Value == uint64Value;
      bool equalFloat32 = float32Value == float32Value;
      bool equalFloat64 = float64Value == float64Value;
      bool equalState = state == state;
      bool equalPoint = point == point;
      bool equalText = text == text;
      bool equalBox = box == box;
      bool equalValues = values == values;
      return equalBool && equalChar && equalByte && equalInt8 &&
             equalInt16 && equalInt32 && equalInt64 && equalUint8 &&
             equalUint16 && equalUint32 && equalUint64 && equalFloat32 &&
             equalFloat64 && equalState && equalPoint && equalText &&
             equalBox && equalValues;
    }
    func Flow(int32? value, bool enabled): int32 {
      if (value != null && enabled) {
        int32 exact = value;
        return exact;
      }
      if (null == value) { return 0; }
      return value;
    }
    func Loop(int32? value): int32 {
      while (value) { return value; }
      return 0;
    }
  )");

  cloth::DiagnosticEngine diagnostics;
  const cloth::FrontendResult frontend =
      compilation.analyze_frontend(diagnostics);
  test.expect(frontend.is_valid && !diagnostics.has_errors(),
              "nullable equality/flow matrix failed frontend analysis:\n" +
                  messages(diagnostics));
  if (!frontend.is_valid) {
    return;
  }
  const cloth::MirModule mir =
      cloth::lower_to_mir(frontend.hir, frontend.semantics);
  test.expect(cloth::verify_mir(mir, frontend.semantics, diagnostics),
              "nullable equality/flow matrix failed MIR verification");
  std::size_t comparisons = 0;
  for (const cloth::MirFileClass& file : mir.files) {
    for (const cloth::MirCallable& function : file.functions) {
      for (const cloth::MirBasicBlock& block : function.body.blocks) {
        for (const cloth::MirInstruction& instruction : block.instructions) {
          comparisons +=
              std::holds_alternative<cloth::MirNullableEqualInstruction>(
                  instruction.data)
                  ? 1U
                  : 0U;
        }
      }
    }
  }
  test.expect(comparisons >= 15,
              "nullable value equality matrix lost an underlying type case");

  cloth::Compilation invalid;
  add(invalid, "InvalidFlow.co", R"(
    int32? Value;
    func Field(): int32 {
      if (Value) { return Value; }
      return 0;
    }
    func Mutation(int32? value): int32 {
      if (value) {
        value = null;
        return value;
      }
      return 0;
    }
  )");
  cloth::DiagnosticEngine invalid_diagnostics;
  const cloth::FrontendResult invalid_frontend =
      invalid.analyze_frontend(invalid_diagnostics);
  test.expect(!invalid_frontend.is_valid &&
                  invalid_diagnostics.diagnostics().size() >= 2 &&
                  !has_diagnostic(invalid_diagnostics, "internal"),
              "nullable value flow accepted unstable field or mutated proof");
}

void verifier_and_lowering(TestContext& test) {
  cloth::Compilation frontend_compilation;
  add(frontend_compilation, "Values.co", R"(
    func Compare(int16? left, int32? right): bool {
      if (left) { int16 exact = left; }
      int32? widened = left;
      int32 required = right!;
      return widened == right;
    }
  )");
  cloth::DiagnosticEngine frontend_diagnostics;
  const cloth::FrontendResult frontend =
      frontend_compilation.analyze_frontend(frontend_diagnostics);
  test.expect(frontend.is_valid, "nullable verifier fixture is invalid");
  if (!frontend.is_valid) {
    return;
  }

  cloth::MirModule broken =
      cloth::lower_to_mir(frontend.hir, frontend.semantics);
  cloth::MirModule broken_lift = broken;
  bool changed_lift = false;
  for (cloth::MirBasicBlock& block :
       broken_lift.files[0].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      const auto* conversion =
          std::get_if<cloth::MirConvertInstruction>(&instruction.data);
      if (conversion != nullptr &&
          conversion->kind == cloth::MirConversionKind::kLiftNullable) {
        instruction.type = *frontend.semantics.find_type("int16?");
        changed_lift = true;
      }
    }
  }
  cloth::DiagnosticEngine lift_diagnostics;
  test.expect(changed_lift &&
                  !cloth::verify_mir(broken_lift, frontend.semantics,
                                     lift_diagnostics) &&
                  has_diagnostic(lift_diagnostics,
                                 "lifted nullable conversion consumes "
                                 "incompatible types"),
              "MIR verifier accepted a malformed lifted conversion");

  bool changed = false;
  for (cloth::MirBasicBlock& block : broken.files[0].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<cloth::MirNullableEqualInstruction>(
              instruction.data)) {
        instruction.type = *frontend.semantics.find_type("int32");
        changed = true;
      }
    }
  }
  cloth::DiagnosticEngine mir_diagnostics;
  test.expect(
      changed &&
          !cloth::verify_mir(broken, frontend.semantics, mir_diagnostics) &&
          has_diagnostic(mir_diagnostics,
                         "nullable equality does not have type bool"),
      "MIR verifier accepted corrupted nullable equality");

  cloth::MirModule broken_presence =
      cloth::lower_to_mir(frontend.hir, frontend.semantics);
  bool changed_presence = false;
  for (cloth::MirBasicBlock& block :
       broken_presence.files[0].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<cloth::MirIsNonNullInstruction>(
              instruction.data)) {
        instruction.type = *frontend.semantics.find_type("int32");
        changed_presence = true;
        break;
      }
    }
    if (changed_presence) {
      break;
    }
  }
  cloth::DiagnosticEngine presence_diagnostics;
  test.expect(changed_presence &&
                  !cloth::verify_mir(broken_presence, frontend.semantics,
                                     presence_diagnostics) &&
                  has_diagnostic(presence_diagnostics,
                                 "non-null test does not have type bool"),
              "MIR verifier accepted a malformed presence test");

  cloth::MirModule broken_assertion =
      cloth::lower_to_mir(frontend.hir, frontend.semantics);
  bool changed_assertion = false;
  for (cloth::MirBasicBlock& block :
       broken_assertion.files[0].functions[0].body.blocks) {
    for (cloth::MirInstruction& instruction : block.instructions) {
      if (std::holds_alternative<cloth::MirNullAssertInstruction>(
              instruction.data)) {
        instruction.type = *frontend.semantics.find_type("int16");
        changed_assertion = true;
        break;
      }
    }
    if (changed_assertion) {
      break;
    }
  }
  cloth::DiagnosticEngine assertion_diagnostics;
  test.expect(
      changed_assertion &&
          !cloth::verify_mir(broken_assertion, frontend.semantics,
                             assertion_diagnostics) &&
          has_diagnostic(assertion_diagnostics,
                         "non-null assertion consumes an incompatible value"),
      "MIR verifier accepted a malformed non-null assertion");

  cloth::Compilation native_compilation;
  add(native_compilation, "Values.co", R"(
    func Compare(int16? left, int32? right): bool {
      int32? widened = left;
      return widened == right;
    }
  )");
  cloth::DiagnosticEngine native_diagnostics;
  const cloth::CompilationResult native =
      native_compilation.analyze(native_diagnostics);
  test.expect(native.is_valid, "nullable value source failed ABI lowering: " +
                                   messages(native_diagnostics));
  if (!native.is_valid) return;
  cloth::DiagnosticEngine llvm_diagnostics;
  const auto llvm = cloth::emit_llvm_ir(native.mir, native.abi,
                                        native.semantics, llvm_diagnostics);
  test.expect(llvm.has_value() && !llvm_diagnostics.has_errors() &&
                  llvm->text.contains("@.cloth.struct.equal.") &&
                  llvm->text.contains("@llvm.memset.p0.i64"),
              "nullable value source failed LLVM lowering: " +
                  messages(llvm_diagnostics));
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"nullable value surface", nullable_value_surface},
      {"safe operations", safe_operations},
      {"invalid contract", invalid_contract},
      {"value equality and flow", value_equality_and_flow},
      {"verifier and lowering", verifier_and_lowering},
  };
  return cloth::test::run_tests(tests);
}
