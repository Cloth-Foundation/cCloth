// Part of the Cloth Compiler project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.txt in the project root for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "cloth/ast/ast_printer.h"

#include "cloth/sema/visibility.h"

#include <ostream>

namespace cloth {
namespace {

void print_type(const TypeSyntax& type, std::ostream& output) {
  output << type.name;
  if (type.is_element_nullable) {
    output << '?';
  }
  if (type.is_array) {
    output << "[]";
  }
  if (type.is_nullable) {
    output << '?';
  }
}

void print_parameters(const std::vector<ParameterDecl>& parameters,
                      std::ostream& output) {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    if (parameters[index].is_final) {
      output << "final ";
    }
    print_type(parameters[index].type, output);
    output << ' ' << parameters[index].name;
  }
}

void print_validity(bool is_valid, std::ostream& output) {
  if (!is_valid) {
    output << " [invalid]";
  }
}

}  // namespace

void print_ast_summary(const FileClassDecl& file_class, std::ostream& output) {
  output << (file_class.kind == FileTypeKind::kInterface ? "Interface "
                                                         : "FileClass ")
         << file_class.qualified_name;
  if (file_class.base_class) {
    output << " : ";
    print_type(*file_class.base_class, output);
  }
  if (!file_class.interfaces.empty()) {
    output << (file_class.kind == FileTypeKind::kInterface ? " : " : " is ");
    for (std::size_t index = 0; index < file_class.interfaces.size(); ++index) {
      if (index != 0) {
        output << ", ";
      }
      print_type(file_class.interfaces[index], output);
    }
  }
  output << " [" << visibility_name(file_class.visibility);
  if (file_class.is_abstract) {
    output << ", abstract";
  }
  if (file_class.is_sealed) {
    output << ", sealed";
  }
  output << ']';
  print_validity(file_class.is_valid, output);
  output << '\n';

  for (const ImportDecl& import : file_class.imports) {
    output << "|- Import ";
    if (!import.package_name.empty()) {
      output << import.package_name;
    }
    if (import.kind == ImportKind::kWildcard) {
      output << ".*";
    } else {
      if (!import.package_name.empty()) {
        output << "::";
      }
      output << import.type_name;
      if (import.local_name != import.type_name) {
        output << " as " << import.local_name;
      }
    }
    print_validity(import.is_valid, output);
    output << '\n';
  }

  for (const MemberReference member : file_class.member_order) {
    output << "|- ";
    switch (member.kind) {
      case DeclarationKind::kField: {
        const FieldDecl& field = file_class.fields[member.index];
        output << "Field " << field.name << ": ";
        if (field.is_static) {
          output << "static ";
        }
        if (field.is_final) {
          output << "final ";
        }
        print_type(field.type, output);
        output << " [" << visibility_name(field.visibility) << ']';
        print_validity(field.is_valid, output);
        break;
      }
      case DeclarationKind::kFunction: {
        const FunctionDecl& function = file_class.functions[member.index];
        output << "Function " << function.name << '(';
        print_parameters(function.parameters, output);
        output << ')';
        if (function.return_type) {
          output << ": ";
          print_type(*function.return_type, output);
        }
        output << " [" << visibility_name(function.visibility);
        if (function.is_static) {
          output << ", static";
        }
        if (function.is_override) {
          output << ", override";
        }
        if (function.is_abstract) {
          output << ", abstract";
        }
        if (function.is_final) {
          output << ", final";
        }
        output << ']';
        print_validity(function.is_valid, output);
        break;
      }
      case DeclarationKind::kConstructor: {
        const ConstructorDecl& constructor =
            file_class.constructors[member.index];
        output << "Constructor " << constructor.name << '(';
        print_parameters(constructor.parameters, output);
        output << ')';
        if (constructor.initializer) {
          output << " : " << constructor.initializer->base_type.name << " ("
                 << constructor.initializer->arguments.size()
                 << " argument(s))";
        }
        output << " [" << visibility_name(constructor.visibility) << ']';
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
