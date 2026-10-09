// `dplang build` и `dplang test`: нативная программа из C-кода бэкенда,
// runtime и компилятора C (docs/compiler/cli.md, «build» и «test»).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dp {

// Профили сборки (docs/design/ARCHITECTURE.md, раздел 8): проверки одни и
// те же (Р11), различаются оптимизация и санитайзеры.
enum class Profile : uint8_t {
    Debug,    // -O0 -g
    Release,  // -O2
    Test,     // как Debug, с ASan и UBSan, если компилятор C их умеет
};

struct BuildOptions {
    std::string output;       // путь программы (build)
    Profile profile = Profile::Debug;
    std::string runtime_dir;  // каталог runtime/ (core/, linux/)
    std::string cc;           // компилятор C: $CC или cc
};

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
