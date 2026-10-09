// Эталонный интерпретатор Core IR — только для тестов компилятора (Р43;
// docs/ir/README.md, раздел «Интерпретатор»). Исполняет IR по его смыслу
// и строже бэкенда: чтение освобождённого значения и неосвобождённое при
// возврате значение — внутренние ошибки, так проверяется и само понижение.
#pragma once

#include <string>
#include <vector>

#include "ir/ir.h"
#include "support/source.h"

namespace dp::ir {

struct RunResult {
    int exit_code = 0;     // 0 — успех; 1 — main вернула ошибку; 101 — ловушка
    bool internal = false;  // нарушен инвариант IR: message — что именно
    std::string message;
};

// Исполняет main программы с аргументами args (без имени программы,
// os.args); вывод — в stdout и stderr процесса.
RunResult run(const Program& p, const SourceMap& sources, const std::vector<std::string>& args = {});

}  // namespace dp::ir
