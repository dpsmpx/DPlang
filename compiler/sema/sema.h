// Проверка программы: загрузка модулей, имена, сводки, типы
// (docs/spec/core.md, разделы 3–8; docs/compiler/README.md).
#pragma once

#include <memory>
#include <string>

#include "sema/defs.h"
#include "support/diag.h"
#include "support/source.h"
#include "syntax/parser.h"

namespace dp::sema {

// Откуда загрузчик берёт текст модулей: файловая система (CLI) или память
// (worker онлайн-сервиса, compiler/api). Путь — как его строит загрузчик:
// главный модуль; каталог главного модуля + имя + ".dpm"; каталог
// стандартной библиотеки + имя + ".dpm".
class SourceReader {
public:
    virtual ~SourceReader() = default;
    // false — модуля по этому пути нет.
    virtual bool read(const std::string& path, std::string& out) const = 0;
};

struct LoadOptions {
    std::string stdlib_dir;                // каталог стандартной библиотеки; пусто — без неё
    const SourceReader* reader = nullptr;  // nullptr — файловая система
    uint32_t max_nesting = kDefaultMaxNesting;  // предел вложенности разбора (ParseOptions)
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
