// Контракт онлайн-сервиса v1 (Р49; docs/service/README.md, «Контракт»).
//
// Запрос `/api/v1/compile` — JSON-объект с полями language, mode,
// file_name, source. Ответ — JSON с версией API, версиями компилятора и
// языка и либо результатом проверки, либо ошибкой запроса. Ответ
// детерминирован: тот же вход и те же версии — те же байты. Время
// (`meta`) добавляет шлюз. Компилирует общий вход api::check.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dp::api {

inline constexpr std::string_view kApiVersion = "v1";

// Пределы онлайн-профиля (service/config/online-v1.json, раздел limits).
// Значений по умолчанию нет: их задаёт только профиль.
struct OnlineLimits {
    uint32_t request_bytes = 0;     // тело запроса
    uint32_t source_bytes = 0;      // поле source
    uint32_t lines = 0;             // строк в source
    uint32_t identifier_bytes = 0;  // самое длинное имя по лексеру
    uint32_t nesting = 0;           // предел вложенности (E0002)
    uint32_t diagnostics = 0;       // сообщений в ответе
    uint32_t output_bytes = 0;      // ответ целиком
};

// Ответ на тело запроса /api/v1/compile.
std::string compile_v1(std::string_view request, const OnlineLimits& limits);

// Ответ-ошибка: code — код ошибки контракта (invalid_request,
// limit_exceeded, …); limit — имя предела или пусто.
std::string error_v1(std::string_view code, std::string_view message, std::string_view limit = {},
                     uint64_t max = 0);

// Версии: {"api":"v1","compiler":{"version":…,"language":…}} — для
// `dplang worker --describe` и /api/v1/info.
std::string describe_v1();

}  // namespace dp::api
