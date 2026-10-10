// `dpl build` и `dpl test`: нативная программа из C-кода бэкенда,
// runtime и компилятора C (docs/compiler/cli.md, «build» и «test»).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dp {

// Профили сборки (docs/design/ARCHITECTURE.md, раздел 8): проверки одни и
// те же (Р11), различаются оптимизация и санитайзеры.
enum class Profile : uint8_t {
    Debug,    // -O0 -g
    Release,  // -O2
    Test,     // как Debug, с ASan и UBSan, если компилятор C их умеет
};

// Цель сборки (docs/policy/PLATFORMS.md, раздел 4): тройка, порт
// runtime, компилятор C и флаги, запуск программы на хосте.
struct Target {
    std::string_view name;      // --target
    std::string_view triple;    // тройка LLVM
    std::string_view platform;  // порт runtime: каталог runtime/<platform>
    bool host = false;          // компилятор C — $CC или cc; иначе $DPL_CROSS_CC или clang
    bool sanitizers = false;    // профиль test — с ASan и UBSan, если компилятор C их умеет
    std::string_view exe_suffix;
    std::vector<std::string_view> cflags;  // после общих флагов
    std::vector<std::string_view> libs;    // после исходников
    std::string_view runner;    // чем запускать тесты на хосте ($DPL_RUNNER); пусто — сама программа
};

// Цели первой волны; первая — хост.
const std::vector<Target>& targets();
// Цель по имени; nullptr — нет такой.
const Target* find_target(std::string_view name);
// Имена целей через запятую — для сообщений.
std::string target_names();

struct BuildOptions {
    std::string output;       // путь программы (build)
    Profile profile = Profile::Debug;
    std::string runtime_dir;  // каталог runtime/ (core/, <platform>/)
    const Target* target = nullptr;  // nullptr — хост
    std::string cc;           // компилятор C цели (main.cpp: cc_for)
    std::string runner;       // запуск тестов цели; пусто — напрямую
};

// Компилятор C для цели: у хоста — $CC или cc, у других — $DPL_CROSS_CC или clang.
std::string cc_for(const Target& t);
// Запуск тестов цели: $DPL_RUNNER или runner цели.
std::string runner_for(const Target& t);

// Собирает C-код c_code в программу opt.output. Код выхода команды:
// 0 — собрана; 2 — окружение (нет компилятора C или runtime); 3 —
// компилятор C отверг сгенерированный код (внутренняя ошибка).
int build_native(const std::string& c_code, const BuildOptions& opt);

// Собирает C-код сборки тестов (backend_c::emit_c_tests) в профиле test
// и исполняет каждый тест names[i] отдельным процессом. Отчёт — в
// stdout; вывод упавшего теста — после отчёта. Код выхода: 0 — все
// прошли; 1 — есть упавшие; 2 и 3 — как у build_native.
int run_tests(const std::string& c_code, const std::vector<std::string>& names, const BuildOptions& opt);

}  // namespace dp
