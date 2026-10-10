// Общий вход компиляции (Р49; docs/service/README.md, «Одно ядро»).
//
// CLI `dpl`, worker онлайн-сервиса и будущие адаптеры (language
// server, WASM) проверяют программу только через этот вход: своей логики
// компиляции у них нет, поведение одно. Вход не читает ни окружение, ни
// часы; файловую систему — только если модули не даны из памяти.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "sema/sema.h"
#include "support/diag.h"
#include "support/source.h"

namespace dp::api {

// Что делает компиляция. В v0.1 — только проверка; сборка (`build`) —
// шаг 10, через этот же вход.
enum class Mode : uint8_t { Check };

struct CompileOptions {
    Mode mode = Mode::Check;
    // Откуда модули: nullptr — файловая система, иначе — этот источник
    // (например, MemorySources).
    const sema::SourceReader* reader = nullptr;
    std::string stdlib_dir;  // пусто — без стандартной библиотеки
    uint32_t max_nesting = kDefaultMaxNesting;
};

// Проверка программы с главным модулем root_path: загрузка модулей,
// объявления, тела. Если главный модуль не прочитан или в программе ошибки
// разбора, дальше проверка не идёт — сообщения были бы следствием.
// nullptr — главный модуль не прочитан. Диагностика — в diags.
std::unique_ptr<sema::Program> check(const std::string& root_path, const CompileOptions& opt, SourceMap& sources,
                                     Diagnostics& diags);

// Модули из памяти: путь → текст. Других путей нет — файловая система
// не трогается.
class MemorySources : public sema::SourceReader {
public:
    void add(std::string path, std::string text) { files_[std::move(path)] = std::move(text); }
    bool read(const std::string& path, std::string& out) const override;

private:
    std::map<std::string, std::string> files_;
};

}  // namespace dp::api
