#ifndef CLOTH_HIR_HIR_PRINTER_H_
#define CLOTH_HIR_HIR_PRINTER_H_

#include "cloth/hir/hir.h"
#include "cloth/sema/semantic_model.h"

#include <iosfwd>

namespace cloth {

void print_hir_summary(const HirModule& hir, const SemanticModel& semantics,
                       std::ostream& output);

}  // namespace cloth

#endif  // CLOTH_HIR_HIR_PRINTER_H_
