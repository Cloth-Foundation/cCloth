#ifndef CLOTH_AST_AST_PRINTER_H_
#define CLOTH_AST_AST_PRINTER_H_

#include "cloth/ast/ast.h"

#include <iosfwd>

namespace cloth {

void print_ast_summary(const FileClassDecl& file_class, std::ostream& output);

}  // namespace cloth

#endif  // CLOTH_AST_AST_PRINTER_H_
