// Понижение проверенной программы в Core IR (docs/ir/README.md).
#pragma once

#include <memory>

#include "ir/ir.h"
#include "sema/defs.h"

namespace dp::ir {

// Программа должна пройти проверку без ошибок: понижение опирается на
// пометки проверки типов в AST (типы, преобразования, разрешённые имена).
std::unique_ptr<Program> lower(sema::Program& prog);

}  // namespace dp::ir
