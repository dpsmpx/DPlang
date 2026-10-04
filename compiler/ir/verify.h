// Верификатор Core IR (docs/ir/README.md, раздел «Верификатор»):
// проверяет инварианты, на которые опираются бэкенды и интерпретатор.
#pragma once

#include <string>
#include <vector>

#include "ir/ir.h"

namespace dp::ir {

// Нарушения инвариантов; пусто — программа корректна.
std::vector<std::string> verify(const Program& p);

}  // namespace dp::ir
