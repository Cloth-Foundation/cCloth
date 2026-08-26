#include "cloth/ast/ast_printer.h"

#include "cloth/sema/visibility.h"

#include <ostream>

namespace cloth {
namespace {

void print_parameters(const std::vector<ParameterDecl>& parameters,
                      std::ostream& output) {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << parameters[index].type.name << ' ' << parameters[index].name;
  }
}

void print_validity(bool is_valid, std::ostream& output) {
  if (!is_valid) {
    output << " [invalid]";
  }
}

}  // namespace

void print_ast_summary(const FileClassDecl& file_class, std::ostream& output) {
  output << "FileClass " << file_class.name << " ["
         << visibility_name(file_class.visibility) << ']';
  print_validity(file_class.is_valid, output);
  output << '\n';

  for (const MemberReference member : file_class.member_order) {
    output << "|- ";
    switch (member.kind) {
      case DeclarationKind::kField: {
        const FieldDecl& field = file_class.fields[member.index];
        output << "Field " << field.name << ": " << field.type.name << " ["
               << visibility_name(field.visibility) << ']';
        print_validity(field.is_valid, output);
        break;
      }
      case DeclarationKind::kFunction: {
        const FunctionDecl& function = file_class.functions[member.index];
        output << "Function " << function.name << '(';
        print_parameters(function.parameters, output);
        output << ')';
        if (function.return_type) {
          output << ": " << function.return_type->name;
        }
        output << " [" << visibility_name(function.visibility) << ']';
        print_validity(function.is_valid, output);
        break;
      }
      case DeclarationKind::kConstructor: {
        const ConstructorDecl& constructor =
            file_class.constructors[member.index];
        output << "Constructor " << constructor.name << '(';
        print_parameters(constructor.parameters, output);
        output << ") [" << visibility_name(constructor.visibility) << ']';
        print_validity(constructor.is_valid, output);
        break;
      }
      case DeclarationKind::kNestedType:
        output << "NestedType <unsupported>";
        break;
    }
    output << '\n';
  }
}

}  // namespace cloth
