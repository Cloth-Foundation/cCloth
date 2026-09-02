// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/compiler/compilation.h"
#include "cloth/hir/hir_printer.h"
#include "cloth/hir/hir_verifier.h"
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
      func Moved(int32 dx): Point { return Point(X + dx, Y); }
      func Copy(): Point { return self; }
      func CopyAgain(): Point { return Copy(); }
      static func Origin(): Point { return Point(0, 0); }
    }
  )");
}

void value_frontend(TestContext& test) {
  cloth::Compilation compilation;
  // Consumers intentionally precede declarations.
  add(compilation, "Main.co", R"(
    Point location;
    Main() { location = Point.Origin(); }
    static func Change(Point value): Point { value.X++; return value; }
    static func Run() {
      var point = Point(1, 2);
      Point copy = Change(point);
      copy = point.Moved(3);
      copy.X += 10;
      final Point fixed = copy;
      final Point[] values = [point, fixed.Copy()];
      Point[]? optional = values;
      values[0].X++;
      for (var item in values) { item.X = 7; }
      println(values[0] == point);
      print(Point.Origin());
      println(fixed::typeName);
    }
  )");
  add_point(compilation);
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto mir = cloth::lower_to_mir(result.hir, result.semantics);
  test.expect(cloth::verify_mir(mir, result.semantics, diagnostics),
              messages(diagnostics));
  const auto type = result.semantics.find_type("Point");
  test.expect(
      type && result.semantics.type(*type).kind == cloth::TypeKind::kStruct,
      "struct must have a distinct nominal type");
  if (!type) return;
  const auto& file = result.semantics.file(*result.semantics.type(*type).file);
  test.expect(file.identity.kind == cloth::NominalKind::kStruct &&
                  file.virtual_functions.empty(),
              "struct identity or direct dispatch was lost");
  std::ostringstream summary;
  cloth::print_hir_summary(result.hir, result.semantics, summary);
  test.expect(
      summary.str().find("Struct Point") != std::string::npos &&
          summary.str().find("read-only value receiver") != std::string::npos,
      "HIR summary must retain the value receiver contract");
}

void reference_boundaries(TestContext& test) {
  cloth::Compilation compilation;
  add_point(compilation);
  add(compilation, "Box.co", R"(
    int32 Value;
    Point PointValue;
    Box() { PointValue = Point(0, 0); }
  )");
  add(compilation, "Bundle.co", R"(
    struct {
      Point PointValue;
      Box Object;
      Point[] Points;
      string? Note = null;
      Bundle() { PointValue = Point(0, 0); Object = Box(); Points = [Point(1, 2)]; }
      func Touch() { Object.Value++; Object.PointValue.X++; Points[0].X++; }
      func Changed(Point value): Point { value.X++; return value; }
      static func Main() {
        final Bundle fixed = Bundle();
        fixed.Object.Value++;
        fixed.Object.PointValue.X++;
        fixed.Points[0].X++;
        fixed.Touch();
        Bundle copy = fixed;
        copy.PointValue.X++;
        copy = Bundle();
        println(Bundle().Changed(copy.PointValue).X);
      }
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
}

void invalid_values(TestContext& test) {
  const std::vector<std::string> bodies{
      "Point value;",
      "Point value = 1;",
      "Point? value = null;",
      "Point?[] values = [];",
      "object value = Point(1, 2);",
      "Point value = Other();",
      "Point[] values = [Other()];",
      "var values = [Point(1, 2), Other()];",
      "if (Point(1, 2)) {}",
      "var value = Point(1, 2) is object;",
      "var value = Point(1, 2) as object;",
      "var value = int32(Point(1, 2));",
      "var value = Point(1, 2)!;",
      "var value = Point(1, 2) + Point(3, 4);",
      "var value = Point(1, 2) < Point(3, 4);",
      "var value = Point(1, 2) == Other();",
      "Point value = Point(1, 2); value++;",
      "Point value = Point(1, 2); value += 1;",
      "Point(1, 2).X = 3;",
      "Point.Origin().X++;",
      "(Point(1, 2)).X += 1;",
      "final Point value = Point(1, 2); value.X = 3;",
      "final Point value = Point(1, 2); (value).X++;",
      "final Point value = Point(1, 2); value = Point(3, 4);",
      "Point value = Point(1, 2); value.Y = 3;",
      "var value = Point();",
      "var value = Point::X;"};
  for (const auto& body : bodies) {
    cloth::Compilation compilation;
    add_point(compilation);
    add(compilation, "Other.co", "struct { Other() {} }");
    add(compilation, "Main.co", "static func Main() { " + body + " }");
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze_frontend(diagnostics);
    test.expect(!result.is_valid && diagnostics.has_errors(), body);
    test.expect(messages(diagnostics).find("internal") == std::string::npos,
                "invalid source should receive source diagnostics: " + body);
  }
}

void initialization_and_receivers(TestContext& test) {
  const std::vector<std::string> invalid{
      "int32 X; Value() {}",
      "int32 X; Value(bool b) { if (b) { X = 1; } }",
      "int32 X; Value(bool b) { if (b) { return; } X = 1; }",
      "int32 X; Value() { while (false) { X = 1; } }",
      "int32 X; int32 Y = X; Value() { X = 1; }",
      "int32 X; Value() { println(self); X = 1; }",
      "int32 X; Value() { Touch(); X = 1; } func Touch() {}",
      "Point P; Value() { P.X = 1; }",
      "final Point P; Value() { P = Point(0, 0); P.X = 1; }",
      "int32 X = 0; Value() {} func Change() { X = 1; }",
      "int32 X = 0; Value() {} func Change() { self.X++; }",
      "Point P = Point(0, 0); Value() {} func Change() { P.X += 1; }",
      "int32 X = 0; Value() { self = Value(); }",
      "int32 X = 0; Value() {} func Change() { self = Value(); }",
      "Value() {} abstract func Missing();",
      "Value() {} override func Change() {}",
      "Value() {} final func Change() {}",
      "Value(): Point(1, 2) {}",
      "Value() {} func Change() { super.Change(); }",
      "Value() {} static final Value Default = Value();",
      "Value() {} func Change(final Point p) { p.X++; }",
      "Value() {} func Change(Point[] points) { for (final var p in points) { "
      "p.X = 1; } }",
      "final int32 X; Value() { X = 1; X = 2; }",
      "Value() {} func MissingReturn(): Value {}",
      "Value() {} func Main() {}"};
  for (const auto& members : invalid) {
    cloth::Compilation compilation;
    add_point(compilation);
    add(compilation, "Value.co", "struct { " + members + " }");
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze_frontend(diagnostics);
    test.expect(!result.is_valid && diagnostics.has_errors(), members);
  }
  cloth::Compilation valid;
  add(valid, "Value.co", R"(
    struct {
      int32 X;
      int32 Y;
      Value(bool b) {
        if (b) { X = 1; } else { X = 2; }
        Y = self.X;
        println(self);
      }
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  test.expect(valid.analyze_frontend(diagnostics).is_valid,
              messages(diagnostics));
}

void syntax_and_cycles(TestContext& test) {
  for (const std::string source :
       {"struct Value {}", "abstract struct {}", "sealed struct {}",
        "struct : Point {}", "struct is View {}", "struct {} int32 X;",
        "struct { struct {} }", "struct { func Bodyless(); }"}) {
    cloth::Compilation compilation;
    add_point(compilation);
    add(compilation, "View.co", "interface {}");
    add(compilation, "Value.co", source);
    cloth::DiagnosticEngine diagnostics;
    test.expect(!compilation.analyze_frontend(diagnostics).is_valid, source);
  }
  for (const bool indirect : {false, true}) {
    cloth::Compilation compilation;
    add(compilation, "A.co",
        indirect ? "struct { B Value; }" : "struct { A Value; }");
    if (indirect) add(compilation, "B.co", "struct { A Parent; }");
    cloth::DiagnosticEngine diagnostics;
    test.expect(!compilation.analyze_frontend(diagnostics).is_valid,
                "inline cycle was accepted");
    test.expect(messages(diagnostics)
                        .find(indirect ? "A.Value -> B.Parent -> A"
                                       : "A.Value -> A") != std::string::npos,
                "cycle diagnostic must identify its field path");
  }
  cloth::Compilation valid;
  add(valid, "Node.co",
      "struct { Node[] Children; Node(Node[] children) { Children = children; "
      "} }");
  add(valid, "Empty.co", "struct { Empty() {} }");
  cloth::DiagnosticEngine diagnostics;
  test.expect(valid.analyze_frontend(diagnostics).is_valid,
              messages(diagnostics));
}

void class_and_interface_values(TestContext& test) {
  cloth::Compilation compilation;
  add_point(compilation);
  add(compilation, "Transform.co", "interface { func Apply(Point p): Point; }");
  add(compilation, "Factory.co", R"(
    class is Transform {
      final Point Origin;
      Factory() { Origin = Point(0, 0); }
      func Apply(Point p): Point { p.X += 2; return p; }
      static func Main() {
        Transform transform = Factory();
        println(transform.Apply(Point(1, 2)));
      }
    }
  )");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  for (const std::string members :
       {"Point P; Holder() {}", "Point P; Holder() { P.X = 1; }",
        "final Point P; Holder() { P = Point(0, 0); P.X++; }",
        "Point P = Point(0, 0); static func Check(Holder? holder) { var value "
        "= holder?.P; }"}) {
    cloth::Compilation invalid;
    add_point(invalid);
    add(invalid, "Holder.co", members);
    cloth::DiagnosticEngine errors;
    test.expect(!invalid.analyze_frontend(errors).is_valid, members);
  }
}

void imports_and_visibility(TestContext& test) {
  for (const bool reverse : {false, true}) {
    cloth::Compilation compilation;
    compilation.set_package_dependencies(
        {{"app", "left", "first"}, {"app", "right", "second"}});
    const auto source = [&](std::string package) {
      compilation.add_package_source(
          cloth::SourceFile::from_memory("Value.co",
                                         "struct { int32 X = 1; Value() {} }"),
          std::move(package), "", "1.0.0");
    };
    source(reverse ? "second" : "first");
    source(reverse ? "first" : "second");
    compilation.add_package_source(cloth::SourceFile::from_memory("Main.co", R"(
      import left::Value as Left;
      import right::Value as Right;
      static func Select(Left value): int32 { return value.X; }
      static func Select(Right value): int32 { return value.X; }
      static func Main() { println(Select(Left())); println(Select(Right())); }
    )"),
                                   "app", "", "1.0.0");
    cloth::DiagnosticEngine diagnostics;
    const auto result = compilation.analyze_frontend(diagnostics);
    test.expect(result.is_valid, messages(diagnostics));
    test.expect(result.semantics.find_type("first.Value") !=
                    result.semantics.find_type("second.Value"),
                "package identity collapsed distinct structs");
  }
  for (const std::string body :
       {"var value = Value();", "var value = Value.Make().hidden;",
        "Value.Make().secret();"}) {
    cloth::Compilation compilation;
    add(compilation, "Value.co", R"(
      struct { int32 hidden = 1; value() {} static func Make(): Value { return Value(); }
        func secret() {} }
    )");
    add(compilation, "Main.co", "static func Main() { " + body + " }");
    cloth::DiagnosticEngine diagnostics;
    test.expect(!compilation.analyze_frontend(diagnostics).is_valid &&
                    messages(diagnostics).find("private") != std::string::npos,
                "private struct member escaped visibility: " + body);
  }
  cloth::Compilation compilation;
  add(compilation, "value.co", "struct { value() {} }");
  add(compilation, "Main.co", "static func Main() { var v = value(); }");
  cloth::DiagnosticEngine diagnostics;
  test.expect(!compilation.analyze_frontend(diagnostics).is_valid,
              "lowercase struct filename must remain private");
}

void hir_invariants(TestContext& test) {
  cloth::Compilation compilation;
  add_point(compilation);
  add(compilation, "Other.co", "struct { Other() {} }");
  add(compilation, "Main.co", R"(
    static func Main() { Point p = Point(1, 2); p.X++; p = p.Copy(); println(p == p); }
  )");
  cloth::DiagnosticEngine diagnostics;
  const auto result = compilation.analyze_frontend(diagnostics);
  test.expect(result.is_valid, messages(diagnostics));
  if (!result.is_valid) return;
  const auto point = *result.semantics.find_type("Point");
  const auto other = *result.semantics.find_type("Other");
  for (int mutation = 0; mutation != 7; ++mutation) {
    auto broken = result.hir;
    cloth::HirStorage storage;
    bool changed = false;
    for (auto expression : broken.storage.expressions()) {
      if (!changed) {
        if (mutation == 0 && expression.type == point &&
            expression.category == cloth::ValueCategory::kMutableLocation) {
          expression.type = other;
          changed = true;
        } else if (mutation == 1 && expression.type == point &&
                   expression.category ==
                       cloth::ValueCategory::kReadOnlyLocation) {
          expression.category = cloth::ValueCategory::kMutableLocation;
          changed = true;
        } else if (mutation == 2 &&
                   std::holds_alternative<cloth::HirMemberExpression>(
                       expression.data) &&
                   expression.category ==
                       cloth::ValueCategory::kMutableLocation) {
          expression.category = cloth::ValueCategory::kValue;
          changed = true;
        } else if (auto* call =
                       std::get_if<cloth::HirCallExpression>(&expression.data);
                   mutation == 3 && call &&
                   call->struct_receiver ==
                       cloth::StructReceiverMode::kReadOnlyValue) {
          call->struct_receiver = cloth::StructReceiverMode::kNone;
          changed = true;
        } else if (mutation == 4 && expression.type == point &&
                   std::holds_alternative<cloth::HirAssignmentExpression>(
                       expression.data)) {
          expression.type = other;
          changed = true;
        } else if (auto* binary = std::get_if<cloth::HirBinaryExpression>(
                       &expression.data);
                   mutation == 5 && binary &&
                   binary->operation == cloth::TokenKind::kEqualEqual) {
          binary->operation = cloth::TokenKind::kPlus;
          changed = true;
        } else if (auto* call =
                       std::get_if<cloth::HirCallExpression>(&expression.data);
                   mutation == 6 && call &&
                   call->struct_receiver ==
                       cloth::StructReceiverMode::kReadOnlyValue) {
          const auto type = std::ranges::find_if(
              broken.storage.expressions(), [point](const auto& candidate) {
                return candidate.type == point &&
                       std::holds_alternative<cloth::HirTypeExpression>(
                           candidate.data);
              });
          if (type != broken.storage.expressions().end()) {
            call->callee = cloth::HirExpressionId{static_cast<std::size_t>(
                type - broken.storage.expressions().begin())};
            changed = true;
          }
        }
      }
      static_cast<void>(storage.add_expression(std::move(expression)));
    }
    for (const auto& statement : broken.storage.statements())
      static_cast<void>(storage.add_statement(statement));
    for (const auto& block : broken.storage.blocks())
      static_cast<void>(storage.add_block(block));
    broken.storage = std::move(storage);
    cloth::DiagnosticEngine invalid;
    test.expect(
        changed && !cloth::verify_hir(broken, result.semantics, invalid),
        "HIR accepted struct corruption " + std::to_string(mutation));
  }
  auto broken = result.hir;
  broken.files[0].functions[0].struct_receiver =
      cloth::StructReceiverMode::kNone;
  cloth::DiagnosticEngine receiver_diagnostics;
  test.expect(
      !cloth::verify_hir(broken, result.semantics, receiver_diagnostics),
      "HIR accepted a missing value receiver contract");
  broken = result.hir;
  static_cast<void>(broken.storage.add_expression(
      {point,
       result.semantics.symbol(result.semantics.file(cloth::FileId{0}).symbol)
           .range,
       cloth::HirLiteralExpression{cloth::LiteralKind::kInteger, "0"}}));
  cloth::DiagnosticEngine literal_diagnostics;
  test.expect(!cloth::verify_hir(broken, result.semantics, literal_diagnostics),
              "HIR accepted a scalar struct literal");
}

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"value frontend", value_frontend},
      {"reference boundaries", reference_boundaries},
      {"invalid values", invalid_values},
      {"initialization and receivers", initialization_and_receivers},
      {"syntax and cycles", syntax_and_cycles},
      {"class and interface values", class_and_interface_values},
      {"imports and visibility", imports_and_visibility},
      {"HIR invariants", hir_invariants},
  };
  return cloth::test::run_tests(tests);
}
