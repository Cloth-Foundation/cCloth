#ifndef CLOTH_MIR_MIR_PRINTER_H_
#define CLOTH_MIR_MIR_PRINTER_H_

#include "cloth/mir/mir.h"
#include "cloth/sema/semantic_model.h"

#include <iosfwd>

namespace cloth {

void print_mir_summary(const MirModule& mir, const SemanticModel& semantics,
                       std::ostream& output);

}  // namespace cloth

#endif  // CLOTH_MIR_MIR_PRINTER_H_
