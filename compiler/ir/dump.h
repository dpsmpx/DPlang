// Текстовая запись Core IR для золотых дампов (docs/ir/README.md,
// раздел «Запись»). Формат — для тестов и отладки, не стабилен (S2).
#pragma once

#include <string>

#include "ir/ir.h"
#include "support/source.h"

namespace dp::ir {

std::string dump_program(const Program& p, const SourceMap& sources);
std::string dump_function(const Program& p, const Function& f, const SourceMap& sources);

}  // namespace dp::ir
