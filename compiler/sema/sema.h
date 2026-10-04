// Проверка программы: загрузка модулей, имена, сводки, типы
// (docs/spec/core.md, разделы 3–8; docs/compiler/README.md).
#pragma once

#include <memory>
#include <string>

#include "sema/defs.h"
#include "support/diag.h"
#include "support/source.h"

namespace dp::sema {

struct LoadOptions {
    std::string stdlib_dir;  // каталог стандартной библиотеки; пусто — без неё
};

// Загружает главный модуль и всё, что он импортирует (Р46): разбор, граф
// импортов без циклов, порядок зависимостей. nullptr — главный модуль не
// прочитан.
std::unique_ptr<Program> load_program(const std::string& root_path, const LoadOptions& opt, SourceMap& sources,
                                      Diagnostics& diags);

// Объявления модулей: области имён, типы в сигнатурах, проверка сводок.
void collect(Program& prog, Diagnostics& diags);

// Тела функций, константы, значения полей по умолчанию: имена и типы.
void check(Program& prog, Diagnostics& diags);

// Вход программы: `main` главного модуля без параметров, результат —
// нет или `E!void` (E0322). Нужен для сборки и запуска, не для check.
bool check_main(Program& prog, Diagnostics& diags);

// Сводка модуля — его открытый интерфейс в записи языка.
std::string summary_text(const ModuleInfo& m);

}  // namespace dp::sema
