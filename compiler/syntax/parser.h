// Парсер (docs/spec/core.md, раздел 2): рекурсивный спуск с приоритетами
// операторов. Разбор не зависит от типов (Р7). После ошибки парсер
// пропускает остаток строки или объявления и продолжает, так что одна
// ошибка даёт одно сообщение, а модуль разбирается до конца.
#pragma once

#include <memory>
#include <string>

#include "support/diag.h"
#include "support/source.h"
#include "syntax/ast.h"

namespace dp {

std::unique_ptr<ast::Module> parse_module(const SourceFile& file, FileId id, std::string name, Diagnostics& diags);

}  // namespace dp
