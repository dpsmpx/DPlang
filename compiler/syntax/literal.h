// Значения литералов (docs/spec/core.md, разделы 1.5–1.8).
//
// Лексер проверяет литералы этими же функциями, а построитель AST берёт
// из них значения — правило одно на оба места.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "support/utf8.h"

namespace dp {

enum class LitStatus : uint8_t { Ok, Malformed, OutOfRange };

// Целый литерал: десятичный, 0x, 0b, 0o; `_` — только между цифрами.
LitStatus parse_int_literal(std::string_view text, uint64_t& value);
// Литерал с плавающей точкой: `1.5`, `2e10`, `1_000.5e-3`.
LitStatus parse_float_literal(std::string_view text, double& value);

// Разбор экранирования, начиная с `\` в text[i]. Возвращает длину
// записи и значение (код символа или байта); 0 — запись неверна, тогда
// bad_len — сколько байтов отметить в сообщении.
struct Escape {
    uint32_t len = 0;
    uint32_t value = 0;
    bool is_unicode = false;  // \u{...}: value — код символа, а не байт
    uint32_t bad_len = 0;
};
Escape parse_escape(std::string_view text, size_t i);

// Байты строкового литерала (с кавычками в text) после экранирования.
// Литерал должен быть уже проверен лексером.
std::string string_literal_value(std::string_view text);
// Значение байтового литерала `'a'`; литерал должен быть проверен.
uint8_t byte_literal_value(std::string_view text);

}  // namespace dp
