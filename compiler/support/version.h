// Версии компилятора и языка — одно место для CLI, worker онлайн-сервиса
// и контракта (docs/service/README.md, «Версии»).
#pragma once

#include <string_view>

namespace dp {

// Версия компилятора: меняется с каждой вехой (docs/policy/RELEASE.md).
inline constexpr std::string_view kCompilerVersion = "0.1.0-dev";

// Версия языка, которую принимает компилятор: спецификация ядра
// docs/spec/core.md. После v1.0 — редакции по году (Р37).
inline constexpr std::string_view kLanguageVersion = "0.1";

}  // namespace dp
