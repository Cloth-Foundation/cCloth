// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

#include "abi_names.h"
#include "test.h"

namespace {

using cloth::test::TestCase;
using cloth::test::TestContext;

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

struct CompiledNullableValues {
  explicit CompiledNullableValues(cloth::TargetDataLayout target)
      : compilation(std::move(target)) {
    compilation.add_source(cloth::SourceFile::from_memory(
        "Layout.co",
        "import Payload;\n"
        "import Nested;\n"
        "import Empty;\n"
        "Payload? Value;\n"
        "Nested? NestedValue;\n"
        "Empty? EmptyValue;\n"
        "func Pass(Payload? value): Payload? { return value; }\n"
        "func Number(int32? value): int32? { return value; }\n"
        "func PassNested(Nested? value): Nested? { return value; }\n"
        "func PassEmpty(Empty? value): Empty? { return value; }\n"));
    compilation.add_source(
        cloth::SourceFile::from_memory("Payload.co",
                                       "struct {\n"
                                       "string Text;\n"
                                       "int32 Count;\n"
                                       "Payload(string text, int32 count) {\n"
                                       "  Text = text;\n"
                                       "  Count = count;\n"
                                       "}\n"
                                       "}\n"));
    compilation.add_source(cloth::SourceFile::from_memory(
        "Nested.co",
        "import Payload;\n"
        "struct {\n"
        "Payload? Value;\n"
        "Nested(Payload? value) { Value = value; }\n"
        "}\n"));
    compilation.add_source(
        cloth::SourceFile::from_memory("Empty.co", "struct { Empty() {} }\n"));
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
      file.type_descriptor->kind == cloth::AbiHeapObjectKind::kFileClass &&
          file.type_descriptor->name == "Layout" &&
          file.type_descriptor->size == layout.size &&
          file.type_descriptor->alignment == layout.alignment &&
          file.type_descriptor->reference_offsets ==
              std::vector<std::uint64_t>{32},
      "file-class descriptor lost its identity or reference layout");
}

void inherited_class_layout(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_source(cloth::SourceFile::from_memory(
      "Derived.co", "class : Base {\nint32 Count;\nBase? Owner;\n}\n"));
  compilation.add_source(cloth::SourceFile::from_memory(
      "Base.co", "class {\nstring? Name;\nbyte Tag;\n}\n"));
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);

  test.expect(result.is_valid,
              "valid derived class failed ABI lowering or verification");
  const cloth::AbiFileClass& derived = result.abi.files[0];
  const cloth::AbiFileClass& base = result.abi.files[1];
  test.expect(result.hir.files[0].base_file == cloth::FileId{1} &&
                  result.mir.files[0].base_file == cloth::FileId{1} &&
                  derived.base_file == cloth::FileId{1},
              "base identity was lost between semantic and layout IR");
  test.expect(base.layout.size == 32 && base.layout.fields.size() == 2 &&
                  base.layout.fields[0].offset == 16 &&
                  base.layout.fields[1].offset == 24,
              "base class layout is wrong");
  test.expect(derived.layout.size == 48 && derived.layout.alignment == 8 &&
                  derived.layout.fields.size() == 4 &&
                  derived.layout.fields[0] == base.layout.fields[0] &&
                  derived.layout.fields[1] == base.layout.fields[1] &&
                  derived.layout.fields[2].offset == 32 &&
                  derived.layout.fields[3].offset == 40,
              "derived class did not preserve the complete base prefix");
  test.expect(
      derived.type_descriptor->parent_file == cloth::FileId{1} &&
          derived.type_descriptor->reference_offsets ==
              std::vector<std::uint64_t>{16, 40} &&
          !base.type_descriptor->parent_file,
      "derived descriptor lost its parent or inherited reference metadata");

  cloth::AbiModule broken = result.abi;
  broken.files[0].type_descriptor->parent_file.reset();
  cloth::DiagnosticEngine verifier_diagnostics;
  test.expect(!cloth::verify_abi(broken, result.mir, result.semantics,
                                 verifier_diagnostics),
              "ABI verifier accepted a missing descriptor parent");
  test.expect(has_diagnostic(verifier_diagnostics,
                             "type descriptor parent does not match") ||
                  has_diagnostic(verifier_diagnostics,
                                 "type descriptor does not match"),
              "descriptor parent corruption produced the wrong diagnostic");
}

void virtual_table_layout(TestContext& test) {
  cloth::Compilation compilation;
  compilation.add_source(cloth::SourceFile::from_memory(
      "Base.co",
      "func First(): int32 { return 1; }\n"
      "func Second(string value): string { return value; }\n"
      "func hidden(): int32 { return 0; }\n"));
  compilation.add_source(cloth::SourceFile::from_memory(
      "Derived.co",
      "class : Base {\n"
      "  override func First(): int32 { return 2; }\n"
      "  func Third(): int32 { return 3; }\n"
      "}\n"));
  cloth::DiagnosticEngine diagnostics;
  const cloth::CompilationResult result = compilation.analyze(diagnostics);

  test.expect(result.is_valid, "valid virtual table failed ABI lowering");
  const cloth::AbiTypeDescriptor& base =
      result.abi.files[0].type_descriptor.value();
  const cloth::AbiTypeDescriptor& derived =
      result.abi.files[1].type_descriptor.value();
  const cloth::FileSemantics& base_semantics =
      result.semantics.file(cloth::FileId{0});
  const cloth::FileSemantics& derived_semantics =
      result.semantics.file(cloth::FileId{1});
  test.expect(base.virtual_functions ==
                  std::vector<cloth::SymbolId>{base_semantics.functions[0],
                                               base_semantics.functions[1]},
              "base virtual table has unstable declaration-order slots");
  test.expect(derived.virtual_functions ==
                  std::vector<cloth::SymbolId>{derived_semantics.functions[0],
                                               base_semantics.functions[1],
                                               derived_semantics.functions[1]},
              "derived virtual table did not replace and extend base slots");

  cloth::AbiModule broken = result.abi;
  broken.files[1].type_descriptor->virtual_functions[0] =
      base_semantics.functions[0];
  cloth::DiagnosticEngine verifier_diagnostics;
  test.expect(!cloth::verify_abi(broken, result.mir, result.semantics,
                                 verifier_diagnostics),
              "ABI verifier accepted a corrupted override slot");
  test.expect(
      has_diagnostic(verifier_diagnostics, "type descriptor does not match"),
      "corrupted virtual slot produced the wrong diagnostic");
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
  test.expect(abi.files[0].type_descriptor->reference_offsets ==
                  std::vector<std::uint64_t>{24},
              "wasm32 descriptor has the wrong reference offset");
}

void callable_abi(TestContext& test) {
  const CompiledSource source{
      "Layout(int32 value) {}\n"
      "layout(string value) {}\n"
      "func Build(int32 value): Layout { return Layout(value); }\n"
      "func hidden(): bool { return true; }\n"};
  const cloth::AbiFileClass& file = source.result->abi.files[0];
  const cloth::AbiCallable& build = file.functions[0];
  const cloth::AbiCallable& hidden = file.functions[1];
  const cloth::AbiCallable& constructor = file.constructors[0];
  const cloth::AbiCallable& private_constructor = file.constructors[1];

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
  test.expect(
      constructor.parameters.size() == 1 &&
          constructor.parameters[0].kind ==
              cloth::AbiParameterKind::kExplicit &&
          constructor.return_type ==
              source.result->semantics.file(cloth::FileId{0}).type &&
          constructor.initializer_mangled_name ==
              cloth::test::initializer_name("Layout", "Layout", {"int32"}),
      "constructor allocation or initialization ABI is wrong");
  test.expect(
      private_constructor.linkage == cloth::AbiLinkage::kInternal &&
          private_constructor.initializer_linkage ==
              cloth::AbiLinkage::kInternal &&
          private_constructor.initializer_mangled_name ==
              cloth::test::initializer_name("Layout", "layout", {"string"}),
      "private constructor leaked external ABI linkage");
  test.expect(constructor.initializer_linkage == cloth::AbiLinkage::kExternal,
              "accessible base constructor initializer has internal linkage");
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
                  file.static_fields[0].mangled_name ==
                      cloth::test::static_field_name("Layout", "Version"),
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

  test.expect(functions[0].mangled_name ==
                  cloth::test::function_name("Layout", "Pick", {"int32"}),
              "int32 overload has an unstable mangled name");
  test.expect(functions[1].mangled_name ==
                  cloth::test::function_name("Layout", "Pick", {"bool"}),
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
  test.expect(callable.mangled_name ==
                  cloth::test::function_name("Layout", "First", {"int32[]"}),
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
  test.expect(callable.mangled_name ==
                  cloth::test::function_name("Layout", "Maybe", {"Layout"}),
              "nullable ABI mangling did not erase the source qualifier");
}

void nullable_value_abi(TestContext& test) {
  const CompiledNullableValues x86{cloth::TargetDataLayout::llvm_x86_64()};
  const cloth::CompilationResult& result = *x86.result;
  if (!result.is_valid) {
    std::string message = "valid x86-64 nullable-value ABI failed verification";
    for (const cloth::Diagnostic& diagnostic : x86.diagnostics.diagnostics()) {
      message += ": " + diagnostic.message;
    }
    test.expect(false, message);
    return;
  }
  const cloth::AbiCallable& pass = result.abi.files[0].functions[0];
  const cloth::AbiCallable& number = result.abi.files[0].functions[1];
  const cloth::AbiCallable& pass_nested = result.abi.files[0].functions[2];
  const cloth::AbiCallable& pass_empty = result.abi.files[0].functions[3];
  const cloth::TypeId nullable_payload =
      result.semantics.symbol(pass.symbol).parameter_types[0];
  const cloth::TypeId nullable_int32 =
      result.semantics.symbol(number.symbol).parameter_types[0];
  const cloth::TypeId nullable_nested =
      result.semantics.symbol(pass_nested.symbol).parameter_types[0];
  const cloth::TypeId nullable_empty =
      result.semantics.symbol(pass_empty.symbol).parameter_types[0];
  const cloth::AbiTypeLayout& payload_layout =
      result.abi.types[nullable_payload.value];
  const cloth::AbiTypeLayout& int32_layout =
      result.abi.types[nullable_int32.value];
  const cloth::AbiTypeLayout& nested_layout =
      result.abi.types[nullable_nested.value];
  const cloth::AbiTypeLayout& empty_layout =
      result.abi.types[nullable_empty.value];
  test.expect(
      payload_layout.kind == cloth::AbiTypeKind::kAggregate &&
          payload_layout.storage == cloth::SizeAlignment{24, 8} &&
          payload_layout.reference_offsets == std::vector<std::uint64_t>{8},
      "x86-64 nullable struct does not use the tagged value layout");
  test.expect(int32_layout.kind == cloth::AbiTypeKind::kAggregate &&
                  int32_layout.storage == cloth::SizeAlignment{8, 4} &&
                  int32_layout.reference_offsets.empty(),
              "x86-64 nullable primitive does not use the tagged value layout");
  test.expect(
      nested_layout.kind == cloth::AbiTypeKind::kAggregate &&
          nested_layout.storage == cloth::SizeAlignment{32, 8} &&
          nested_layout.reference_offsets == std::vector<std::uint64_t>{16} &&
          empty_layout.kind == cloth::AbiTypeKind::kAggregate &&
          empty_layout.storage == cloth::SizeAlignment{2, 1} &&
          empty_layout.reference_offsets.empty(),
      "nested or empty nullable struct layout is not canonical");
  test.expect(result.abi.files[0].layout.size == 80 &&
                  result.abi.files[0].type_descriptor->reference_offsets ==
                      std::vector<std::uint64_t>{24, 56},
              "nullable struct field lost its shifted GC reference map");
  test.expect(
      pass.return_mode == cloth::AbiReturnMode::kIndirect &&
          pass.parameters.size() == 3 &&
          pass.parameters[0].kind == cloth::AbiParameterKind::kResult &&
          pass.parameters[0].passing == cloth::AbiPassingMode::kResultPointer &&
          pass.parameters[2].kind == cloth::AbiParameterKind::kExplicit &&
          pass.parameters[2].passing == cloth::AbiPassingMode::kValuePointer,
      "nullable struct callable did not use aggregate passing");

  const auto rejects_corruption = [&](const auto& corrupt) {
    cloth::AbiModule broken = result.abi;
    corrupt(broken);
    cloth::DiagnosticEngine verifier_diagnostics;
    return !cloth::verify_abi(broken, result.mir, result.semantics,
                              verifier_diagnostics);
  };
  test.expect(
      rejects_corruption([&](cloth::AbiModule& broken) {
        broken.types[nullable_payload.value].reference_offsets = {0};
      }) &&
          rejects_corruption([&](cloth::AbiModule& broken) {
            ++broken.types[nullable_payload.value].storage.size;
          }) &&
          rejects_corruption([&](cloth::AbiModule& broken) {
            broken.types[nullable_payload.value].storage.alignment = 3;
          }) &&
          rejects_corruption([&](cloth::AbiModule& broken) {
            broken.types[nullable_payload.value].kind =
                cloth::AbiTypeKind::kReference;
          }) &&
          rejects_corruption([](cloth::AbiModule& broken) {
            broken.files[0].functions[0].return_mode =
                cloth::AbiReturnMode::kDirect;
          }) &&
          rejects_corruption([](cloth::AbiModule& broken) {
            broken.files[0].functions[0].parameters[2].passing =
                cloth::AbiPassingMode::kDirect;
          }),
      "ABI verifier accepted corrupted nullable layout or callable metadata");

  const CompiledNullableValues wasm{cloth::TargetDataLayout::llvm_wasm32()};
  const cloth::CompilationResult& wasm_result = *wasm.result;
  test.expect(wasm_result.is_valid,
              "valid wasm32 nullable-value ABI failed verification");
  const cloth::AbiCallable& wasm_pass = wasm_result.abi.files[0].functions[0];
  const cloth::AbiCallable& wasm_nested = wasm_result.abi.files[0].functions[2];
  const cloth::AbiCallable& wasm_empty = wasm_result.abi.files[0].functions[3];
  const cloth::TypeId wasm_nullable_payload =
      wasm_result.semantics.symbol(wasm_pass.symbol).parameter_types[0];
  const cloth::AbiTypeLayout& wasm_layout =
      wasm_result.abi.types[wasm_nullable_payload.value];
  const cloth::AbiTypeLayout& wasm_nested_layout =
      wasm_result.abi.types[wasm_result.semantics.symbol(wasm_nested.symbol)
                                .parameter_types[0]
                                .value];
  const cloth::AbiTypeLayout& wasm_empty_layout =
      wasm_result.abi.types[wasm_result.semantics.symbol(wasm_empty.symbol)
                                .parameter_types[0]
                                .value];
  test.expect(
      wasm_layout.kind == cloth::AbiTypeKind::kAggregate &&
          wasm_layout.storage == cloth::SizeAlignment{12, 4} &&
          wasm_layout.reference_offsets == std::vector<std::uint64_t>{4} &&
          wasm_nested_layout.storage == cloth::SizeAlignment{16, 4} &&
          wasm_nested_layout.reference_offsets ==
              std::vector<std::uint64_t>{8} &&
          wasm_empty_layout.storage == cloth::SizeAlignment{2, 1} &&
          result.abi.files[0].layout.size == 80 &&
          wasm_result.abi.files[0].layout.size == 40 &&
          wasm_result.abi.files[0].type_descriptor->reference_offsets ==
              std::vector<std::uint64_t>{12, 28},
      "wasm32 nullable struct layouts or reference maps are not canonical");
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
  test.expect(left == cloth::test::function_name("left.User", "Make") &&
                  right == cloth::test::function_name("right.User", "Make"),
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
  broken.files[0].type_descriptor->reference_offsets = {17};
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

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"x86-64 type layout", x86_64_type_layout},
      {"class field layout", class_field_layout},
      {"inherited class layout", inherited_class_layout},
      {"virtual table layout", virtual_table_layout},
      {"wasm32 layout", wasm32_layout},
      {"callable ABI", callable_abi},
      {"static member ABI", static_member_abi},
      {"void ABI", void_abi},
      {"deterministic mangling", deterministic_mangling},
      {"array ABI", array_abi},
      {"nullable ABI", nullable_abi},
      {"nullable value ABI", nullable_value_abi},
      {"package-qualified mangling", package_qualified_mangling},
      {"verifier rejects layout corruption",
       verifier_rejects_layout_corruption},
      {"verifier rejects invalid target", verifier_rejects_invalid_target},
  };

  return cloth::test::run_tests(tests);
}
