// C-бэкенд (docs/compiler/backend_c.md): проверенный Core IR → C11 без
// неопределённого поведения. Сгенерированный файл собирается вместе с
// runtime (runtime/core/, runtime/linux/) закреплённым компилятором C.
#pragma once

#include <string>
#include <vector>

#include "ir/ir.h"
#include "support/source.h"

namespace dp::backend_c {

struct CResult {
    std::string code;
    // Внутренние ошибки: конструкция IR, которую бэкенд не умеет перевести.
    std::vector<std::string> errors;
};

CResult emit_c(const ir::Program& p, const SourceMap& sources);

}  // namespace dp::backend_c
