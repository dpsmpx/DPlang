// `dplang build`: нативная программа из C-кода бэкенда, runtime и
// компилятора C (docs/compiler/cli.md, «build»).
#pragma once

#include <string>

namespace dp {

struct BuildOptions {
    std::string output;       // путь программы
    bool release = false;     // профиль release: -O2; иначе debug: -O0 -g
    std::string runtime_dir;  // каталог runtime/ (core/, linux/)
    std::string cc;           // компилятор C: $CC или cc
};

// Собирает C-код c_code в программу. Код выхода команды: 0 — собрана;
// 2 — окружение (нет компилятора C или runtime); 3 — компилятор C
// отверг сгенерированный код (внутренняя ошибка); сообщение — в stderr.
int build_native(const std::string& c_code, const BuildOptions& opt);

}  // namespace dp
