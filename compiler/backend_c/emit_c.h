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
    std::vector<std::string> links;  // библиотеки C программы (`@link`, Р59): `-l<имя>`
};

CResult emit_c(const ir::Program& p, const SourceMap& sources);

// Сборка тестов (Р50): вместо main — вход `программа N`, который исполняет
// тест tests[N] (номер функции в IR); без аргумента — печатает их число.
CResult emit_c_tests(const ir::Program& p, const SourceMap& sources, const std::vector<uint32_t>& tests);

}  // namespace dp::backend_c
