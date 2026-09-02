// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/abi/abi_verifier.h"
#include "cloth/artifact/imported_package.h"
#include "cloth/artifact/package_artifact.h"
#include "cloth/backend/llvm_ir.h"
#include "cloth/compiler/compilation.h"
#include "cloth/mir/mir.h"
#include "cloth/mir/mir_verifier.h"
#include "cloth/sema/canonical_identity.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "test.h"

namespace {
using cloth::test::TestCase;
using cloth::test::TestContext;

std::string messages(const cloth::DiagnosticEngine& diagnostics) {
  std::string result;
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    result += diagnostic.message + "\n";
  }
  return result;
}

void add(cloth::Compilation& compilation, std::string name, std::string text) {
  compilation.add_source(
      cloth::SourceFile::from_memory(std::move(name), std::move(text)));
}

void add_point(cloth::Compilation& compilation) {
  add(compilation, "Point.co", R"(
    struct {
      int32 X;
      final int32 Y;
      Point(int32 x, int32 y) { X = x; Y = y; }
    }
  )");
}

void aggregate_abi(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "Holder.co", "byte Lead; Envelope Value = Envelope();");
  add(compilation, "Envelope.co",
      "struct { Label First = Label(); uint64 Count = 0; "
      "Label Second = Label(); Envelope() {} }");
  add(compilation, "Label.co",
      "struct { byte Tag = 0; string Text = \"\"; uint32 Code = 0; Label() {} "
      "}");
  add(compilation, "Empty.co", "struct { Empty() {} }");
  add(compilation, "Data.co",
      "struct { uint64 CountTo; Data(uint64 countTo) { CountTo = countTo; } "
      "func GetCountTo(): uint64 { return self.CountTo; } "
      "static func Echo(Data value): Data { return value; } }");
  cloth::DiagnosticEngine diagnostics;
  const auto frontend = compilation.analyze_frontend(diagnostics);
  test.expect(frontend.is_valid, messages(diagnostics));
  if (!frontend.is_valid) return;
  const auto mir = cloth::lower_to_mir(frontend.hir, frontend.semantics);
  test.expect(cloth::verify_mir(mir, frontend.semantics, diagnostics),
              messages(diagnostics));
  for (const bool wasm : {false, true}) {
    const auto target = wasm ? cloth::TargetDataLayout::llvm_wasm32()
                             : cloth::TargetDataLayout::llvm_x86_64();
    const auto abi =
        cloth::lower_to_abi(mir, frontend.semantics, target, diagnostics);
    test.expect(abi.has_value(), messages(diagnostics));
    if (!abi) continue;
    test.expect(cloth::verify_abi(*abi, mir, frontend.semantics, diagnostics),
                messages(diagnostics));
    const auto& holder = abi->files[0];
    const auto& envelope = abi->files[1];
    const auto& label = abi->files[2];
    const auto& empty = abi->files[3];
    const auto& data = abi->files[4];
    const auto& map =
        abi->types[frontend.semantics.file(envelope.file).type.value];
    test.expect(envelope.layout.size == (wasm ? 40U : 56U) &&
                    envelope.layout.alignment == 8 &&
                    label.layout.size == (wasm ? 12U : 24U) &&
                    empty.layout.size == 1 && empty.layout.alignment == 1 &&
                    data.layout.size == 8 && data.layout.fields[0].offset == 0,
                "aggregate layout differs from the reviewed target vectors");
    test.expect(!envelope.type_descriptor && !label.type_descriptor &&
                    !empty.type_descriptor && !data.type_descriptor &&
                    envelope.layout.header_size == 0,
                "struct acquired a heap header or descriptor");
    test.expect(map.kind == cloth::AbiTypeKind::kAggregate &&
                    map.reference_offsets ==
                        (wasm ? std::vector<std::uint64_t>{4, 28}
                              : std::vector<std::uint64_t>{8, 40}) &&
                    holder.layout.size == (wasm ? 56U : 80U) &&
                    holder.type_descriptor->reference_offsets ==
                        (wasm ? std::vector<std::uint64_t>{20, 44}
                              : std::vector<std::uint64_t>{32, 64}),
                "nested fields lost their precise reference maps");
    const auto& constructor = data.constructors[0];
    const auto& getter = data.functions[0];
    const auto& echo = data.functions[1];
    test.expect(constructor.return_mode == cloth::AbiReturnMode::kIndirect &&
                    constructor.receiver_mode ==
                        cloth::AbiReceiverMode::kConstruction &&
                    constructor.initializer_mangled_name.empty() &&
                    constructor.parameters.size() == 2 &&
                    constructor.parameters[0].kind ==
                        cloth::AbiParameterKind::kResult &&
                    !constructor.parameters[0].symbol &&
                    constructor.parameters[0].passing ==
                        cloth::AbiPassingMode::kResultPointer,
                "struct constructor does not use caller-owned result storage");
    test.expect(
        getter.receiver_mode == cloth::AbiReceiverMode::kReadOnlyValue &&
            getter.return_mode == cloth::AbiReturnMode::kDirect &&
            getter.parameters[0].passing ==
                cloth::AbiPassingMode::kValuePointer &&
            echo.receiver_mode == cloth::AbiReceiverMode::kNone &&
            echo.return_mode == cloth::AbiReturnMode::kIndirect &&
            echo.parameters[1].passing == cloth::AbiPassingMode::kValuePointer,
        "aggregate receiver, parameter, or return passing mode was lost");
    for (int mutation = 0; mutation != 4; ++mutation) {
      auto broken = *abi;
      if (mutation == 0) broken.types[map.type.value].reference_offsets = {0};
      if (mutation == 1)
        broken.files[4].constructors[0].parameters[0].passing =
            cloth::AbiPassingMode::kDirect;
      if (mutation == 2)
        broken.files[4].type_descriptor = holder.type_descriptor;
      if (mutation == 3)
        broken.files[4].functions[0].receiver_mode =
            cloth::AbiReceiverMode::kReference;
      cloth::DiagnosticEngine invalid;
      test.expect(!cloth::verify_abi(broken, mir, frontend.semantics, invalid),
                  "ABI verifier accepted aggregate corruption");
    }
  }
}

void aggregate_size_limit(TestContext& test) {
  cloth::Compilation compilation;
  add(compilation, "S0.co", "struct { uint64 Value = 0; S0() {} }");
  for (int index = 1; index <= 18; ++index) {
    const std::string name = "S" + std::to_string(index);
    const std::string previous = "S" + std::to_string(index - 1);
    add(compilation, name + ".co",
        "struct { " + previous + " Left = " + previous + "(); " + previous +
            " Right = " + previous + "(); " + name + "() {} }");
  }
  cloth::DiagnosticEngine diagnostics;
  const auto frontend = compilation.analyze_frontend(diagnostics);
  test.expect(frontend.is_valid, messages(diagnostics));
  if (!frontend.is_valid) return;
  const auto mir = cloth::lower_to_mir(frontend.hir, frontend.semantics);
  const auto abi =
      cloth::lower_to_abi(mir, frontend.semantics,
                          cloth::TargetDataLayout::llvm_x86_64(), diagnostics);
  test.expect(!abi && messages(diagnostics).contains("padded-size limit"),
              "compact exponentially large aggregate layout was not bounded");
}

void imported_aggregates(TestContext& test) {
  for (const auto& target : {cloth::TargetDataLayout::llvm_x86_64(),
                             cloth::TargetDataLayout::llvm_wasm32()}) {
    cloth::Compilation producer{target};
    producer.add_package_source(cloth::SourceFile::from_memory("Data.co", R"(
      struct {
        byte tag = 1;
        string Text;
        uint64 Count;
        Data(string text, uint64 count) { Text = text; Count = count; }
        func Copy(): Data { return self; }
      }
    )"),
                                "models", "", "1.0.0");
    producer.add_package_source(cloth::SourceFile::from_memory("Packet.co", R"(
      struct {
        Data Value;
        string Tail;
        Packet(Data value) { Value = value; Tail = "tail"; }
        func Copy(): Packet { return self; }
      }
    )"),
                                "models", "", "1.0.0");
    cloth::DiagnosticEngine diagnostics;
    const auto compiled = producer.analyze(diagnostics);
    test.expect(compiled.is_valid, messages(diagnostics));
    if (!compiled.is_valid) continue;
    const auto exported = cloth::build_imported_package_view(
        {"models", "1.0.0"}, compiled.semantics, compiled.mir, compiled.abi);
    for (const auto& issue : exported.issues) test.expect(false, issue.message);
    if (!exported.view) continue;
    const auto& view = *exported.view;
    cloth::PackageArtifact artifact{
        cloth::PackageArtifactKind::kInterface,
        {cloth::kCompilerAbiVersion, cloth::kRuntimeAbiVersion,
         cloth::sha256("aggregate-fixture"), target, std::nullopt},
        {{"Data.co", cloth::sha256("Data")},
         {"Packet.co", cloth::sha256("Packet")}},
        {},
        view,
        {},
        {}};
    for (const auto& file : view.files) {
      test.expect(!file.abi.descriptor, "struct exported a heap descriptor");
      for (const auto& callable : file.abi.callables) {
        test.expect(!callable.initializer_identity,
                    "struct exported a separate constructor initializer");
        artifact.symbols.push_back(
            {callable.mangled_name, callable.member_identity,
             cloth::ArtifactSymbolRole::kDefinition,
             cloth::ArtifactSymbolKind::kCallable,
             cloth::imported_callable_signature(
                 callable.return_mode, callable.receiver_mode,
                 callable.return_type_identity, callable.parameters)});
      }
    }
    std::ranges::sort(artifact.symbols, {}, &cloth::ArtifactSymbol::link_name);
    const auto encoded = cloth::write_package_artifact(artifact);
    for (const auto& issue : encoded.issues) test.expect(false, issue.message);
    if (!encoded.artifact) continue;
    const bool native = target.pointer.size == 8;
    const std::string_view digest =
        native
            ? "15a1691027d990dcfef67eea4f8a2495c47a9a74c981b40ed737a86da84ba264"
            : "950f5053680d60ec7beceec9f741f49a630c277b1ac5c5e0d3a3248ca532c8f"
              "6";
    test.expect(
        encoded.artifact->bytes.size() == (native ? 30464U : 30462U) &&
            cloth::artifact_digest_hex(encoded.artifact->digest) == digest,
        "aggregate format-3 fixture " + target.target_name +
            ": size=" + std::to_string(encoded.artifact->bytes.size()) +
            " digest=" + cloth::artifact_digest_hex(encoded.artifact->digest));
    const auto decoded = cloth::read_package_artifact(encoded.artifact->bytes);
    test.expect(decoded.is_valid() && decoded.artifact->imported == view,
                "aggregate artifact round trip changed its owned metadata");
    if (!decoded.artifact) continue;
    const auto reencoded = cloth::write_package_artifact(*decoded.artifact);
    test.expect(reencoded.is_valid() &&
                    reencoded.artifact->bytes == encoded.artifact->bytes,
                "aggregate artifact is not byte deterministic");

    for (int mutation = 0; mutation < 8; ++mutation) {
      auto broken = view;
      auto& data = *std::ranges::find_if(broken.files, [](const auto& file) {
        return file.nominal_identity.name == "Data";
      });
      auto& type = *std::ranges::find(broken.types, data.identity,
                                      &cloth::ImportedType::identity);
      if (mutation == 0) type.reference_offsets.clear();
      if (mutation == 1) data.abi.fields.erase(data.abi.fields.begin());
      if (mutation == 2) ++data.abi.fields[1].offset;
      if (mutation == 3)
        data.abi.callables[0].parameters[0].passing =
            cloth::AbiPassingMode::kDirect;
      if (mutation == 4)
        data.abi.callables[0].receiver_mode =
            cloth::AbiReceiverMode::kReference;
      if (mutation == 5) data.abi.size = type.storage.size = 1'048'577;
      if (mutation == 6) type.reference_offsets = {1};
      if (mutation == 7) {
        const auto& packet =
            *std::ranges::find_if(broken.files, [](const auto& file) {
              return file.nominal_identity.name == "Packet";
            });
        auto& field = data.abi.fields.front();
        field.type_identity = packet.identity;
        auto& member = *std::ranges::find(data.members, field.field_identity,
                                          &cloth::ImportedMember::identity);
        member.type_identity = packet.identity;
      }
      const auto issues = cloth::verify_imported_package_view(broken);
      test.expect(!issues.empty(),
                  "import verifier accepted aggregate metadata corruption");
      if (mutation == 5) {
        auto oversized = artifact;
        oversized.imported = broken;
        const auto rejected = cloth::write_package_artifact(oversized);
        test.expect(std::ranges::any_of(
                        rejected.issues,
                        [](const auto& issue) {
                          return issue.code ==
                                 cloth::ArtifactIssueCode::kLimitExceeded;
                        }),
                    "oversized aggregate artifact lost its limit diagnostic");
      }
      if (mutation == 7) {
        test.expect(std::ranges::any_of(issues,
                                        [](const auto& issue) {
                                          return issue.message.find("cycle") !=
                                                 std::string::npos;
                                        }),
                    "imported inline layout cycle was not diagnosed");
      }
    }

    cloth::Compilation consumer{target};
    consumer.add_imported_package(decoded.artifact->imported);
    consumer.set_package_dependencies({{"app", "models", "models"}});
    consumer.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
      import models::Data;
      import models::Packet;
      Packet Value = Packet(Data("hello", 100));
      Main() {}
      static func Run() {
        var value = Packet(Data("hello", 100));
        value.Value.Count++;
        var copy = value.Copy();
        println(copy == value);
        println(copy);
        println(copy::typeName);
      }
    )"),
                                "app", "", "1.0.0");
    cloth::DiagnosticEngine consumer_diagnostics;
    const auto consumed = consumer.analyze(consumer_diagnostics);
    test.expect(consumed.is_valid, messages(consumer_diagnostics));
    if (!consumed.is_valid) continue;
    cloth::LlvmIrOptions options;
    options.package = cloth::PackageIdentity{"app", "1.0.0"};
    const auto llvm =
        cloth::emit_llvm_ir(consumed.mir, consumed.abi, consumed.semantics,
                            consumer_diagnostics, options);
    test.expect(llvm.has_value(), messages(consumer_diagnostics));
    const auto app = cloth::build_imported_package_view(
        {"app", "1.0.0"}, consumed.semantics, consumed.mir, consumed.abi);
    test.expect(app.is_valid(), "consumer aggregate ABI failed export");
    if (!app.view) continue;
    auto untrusted = *app.view;
    const auto claimed = std::ranges::find_if(
        untrusted.types,
        [](const auto& type) { return type.kind == cloth::TypeKind::kStruct; });
    if (claimed != untrusted.types.end()) claimed->reference_offsets.clear();
    const cloth::ImportedPackageView* closure[]{&view, &untrusted};
    test.expect(!cloth::verify_imported_package_closure(closure).empty(),
                "dependency-owned reference-map forgery was accepted");
  }
}

void aggregate_resource_limits(TestContext& test) {
  for (int scenario = 0; scenario < 4; ++scenario) {
    cloth::Compilation compilation;
    const bool references = scenario == 1 || scenario == 2;
    add(compilation, "S0.co",
        references ? "struct { string Value = \"root\"; S0() {} }"
                   : "struct { uint64 Value = 0; S0() {} }");
    const int last = scenario == 0   ? 128
                     : scenario == 1 ? 17
                     : scenario == 2 ? 15
                                     : 16;
    for (int index = 1; index <= last; ++index) {
      const auto name = "S" + std::to_string(index);
      const auto previous = "S" + std::to_string(index - 1);
      std::string source = "struct { " + previous + " A = " + previous + "(); ";
      if (scenario != 0) source += previous + " B = " + previous + "(); ";
      add(compilation, name + ".co", source + name + "() {} }");
    }
    if (scenario == 2) {
      for (int index = 0; index < 33; ++index) {
        const auto name = "Holder" + std::to_string(index);
        add(compilation, name + ".co", "S15 Value = S15(); " + name + "() {}");
      }
    }
    if (scenario == 3) {
      add(compilation, "Main.co", "static func Run() { var value = S16(); }");
    }
    cloth::DiagnosticEngine diagnostics;
    const auto frontend = compilation.analyze_frontend(diagnostics);
    test.expect(frontend.is_valid, messages(diagnostics));
    if (!frontend.is_valid) continue;
    const auto mir = cloth::lower_to_mir(frontend.hir, frontend.semantics);
    const auto abi = cloth::lower_to_abi(mir, frontend.semantics,
                                         cloth::TargetDataLayout::llvm_x86_64(),
                                         diagnostics);
    if (scenario == 3) {
      test.expect(abi.has_value(), messages(diagnostics));
      if (!abi) continue;
      const auto llvm =
          cloth::emit_llvm_ir(mir, *abi, frontend.semantics, diagnostics);
      test.expect(
          !llvm && messages(diagnostics).contains("aggregate frame"),
          "aggregate frame budget did not reject oversized backing storage");
    } else {
      const std::string_view expected = scenario == 0   ? "nesting limit"
                                        : scenario == 1 ? "reference-slot limit"
                                                        : "reference-map";
      test.expect(!abi && messages(diagnostics).contains(expected),
                  "aggregate resource boundary: " + messages(diagnostics));
    }
  }
  cloth::Compilation compilation;
  add_point(compilation);
  cloth::DiagnosticEngine diagnostics;
  const auto frontend = compilation.analyze_frontend(diagnostics);
  auto mir = cloth::lower_to_mir(frontend.hir, frontend.semantics);
  mir.files[0].fields.resize(65'537, mir.files[0].fields[0]);
  const auto abi =
      cloth::lower_to_abi(mir, frontend.semantics,
                          cloth::TargetDataLayout::llvm_x86_64(), diagnostics);
  test.expect(!abi && messages(diagnostics).contains("instance-field limit"),
              "aggregate field-count boundary was not enforced");
}

void mir_aggregate_invariants(TestContext& test) {
  cloth::Compilation compilation;
  add_point(compilation);
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto broken = result.mir;
    auto& body = broken.files[0].constructors[0].body;
    auto& instructions = body.blocks[0].instructions;
    auto store =
        std::ranges::find_if(instructions, [](const auto& instruction) {
          return std::holds_alternative<cloth::MirStoreStorageInstruction>(
              instruction.data);
        });
    test.expect(store != instructions.end(), "missing storage-path fixture");
    if (store == instructions.end()) return;
    if (mutation == 0) instructions.erase(store);
    if (mutation == 1) {
      const auto final_store =
          std::ranges::find_if(instructions, [&](const auto& instruction) {
            const auto* value = std::get_if<cloth::MirStoreStorageInstruction>(
                &instruction.data);
            return value &&
                   result.semantics.symbol(value->path.fields[0]).is_final;
          });
      const auto duplicate = *final_store;
      instructions.push_back(duplicate);
    }
    if (mutation == 2) {
      instructions.insert(
          instructions.begin(),
          cloth::MirInstruction{cloth::MirValueId{body.value_count++},
                                result.semantics.files()[0].type, body.range,
                                cloth::MirLoadSymbolInstruction{
                                    result.semantics.files()[0].self_symbol}});
    }
    if (mutation == 3) {
      auto& path =
          std::get<cloth::MirStoreStorageInstruction>(store->data).path;
      path.object = cloth::MirValueId{0};
    }
    if (mutation == 4) {
      auto& path =
          std::get<cloth::MirStoreStorageInstruction>(store->data).path;
      path.fields.clear();
    }
    cloth::DiagnosticEngine invalid;
    test.expect(
        !cloth::verify_mir(broken, result.semantics, invalid),
        "MIR verifier accepted incomplete construction or invalid storage");
  }
}

void aggregate_phi_invariants(TestContext& test) {
  cloth::Compilation compilation;
  add_point(compilation);
  add(compilation, "Select.co", R"(
    static func Pick(bool choose, Point a, Point b): Point {
      if (choose) { return a; }
      return b;
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  auto& function = result.mir.files[1].functions[0];
  const auto range = function.body.range;
  const auto point = *result.semantics.find_type("Point");
  const auto boolean = *result.semantics.find_type("bool");
  using cloth::MirBlockId;
  using cloth::MirValueId;
  function.body = cloth::MirBody{
      range,
      MirBlockId{0},
      {
          {true,
           {{MirValueId{0}, boolean, range,
             cloth::MirLoadSymbolInstruction{function.parameters[0]}},
            {MirValueId{1}, point, range,
             cloth::MirLoadSymbolInstruction{function.parameters[1]}},
            {MirValueId{2}, point, range,
             cloth::MirLoadSymbolInstruction{function.parameters[2]}}},
           {range, cloth::MirBranchTerminator{MirValueId{0}, MirBlockId{1},
                                              MirBlockId{2}}}},
          {true, {}, {range, cloth::MirJumpTerminator{MirBlockId{3}}}},
          {true, {}, {range, cloth::MirJumpTerminator{MirBlockId{3}}}},
          {true,
           {{MirValueId{3}, point, range,
             cloth::MirPhiInstruction{{{MirBlockId{1}, MirValueId{1}},
                                       {MirBlockId{2}, MirValueId{2}}}}}},
           {range, cloth::MirReturnTerminator{MirValueId{3}}}},
      },
      4};
  test.expect(cloth::verify_mir(result.mir, result.semantics, diagnostics),
              messages(diagnostics));
  for (int mutation = 0; mutation < 5; ++mutation) {
    auto broken = result.mir;
    auto& body = broken.files[1].functions[0].body;
    auto& instructions = body.blocks[3].instructions;
    auto& phi = std::get<cloth::MirPhiInstruction>(instructions[0].data);
    if (mutation == 0) phi.incoming.pop_back();
    if (mutation == 1) phi.incoming[1].predecessor = MirBlockId{1};
    if (mutation == 2) phi.incoming[1].predecessor = MirBlockId{0};
    if (mutation == 3) phi.incoming[1].value = MirValueId{0};
    if (mutation == 4) {
      instructions.insert(
          instructions.begin(),
          {MirValueId{body.value_count++}, boolean, range,
           cloth::MirLiteralInstruction{cloth::LiteralKind::kBoolean, "true"}});
    }
    cloth::DiagnosticEngine invalid;
    test.expect(
        !cloth::verify_mir(broken, result.semantics, invalid),
        "MIR accepted malformed aggregate phi " + std::to_string(mutation));
  }
}

void lowering_boundary(TestContext& test) {
  cloth::Compilation compilation;
  add_point(compilation);
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  test.expect(!result.mir.files.empty() && !result.abi.types.empty(),
              "struct must lower through MIR and aggregate ABI");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"aggregate ABI", aggregate_abi},
      {"aggregate size limit", aggregate_size_limit},
      {"imported aggregates", imported_aggregates},
      {"aggregate resource limits", aggregate_resource_limits},
      {"aggregate phi invariants", aggregate_phi_invariants},
      {"MIR aggregate invariants", mir_aggregate_invariants},
      {"lowering boundary", lowering_boundary},
  };
  return cloth::test::run_tests(tests);
}
