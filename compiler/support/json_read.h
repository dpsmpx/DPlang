// Строгий разбор JSON (RFC 8259) для входа онлайн-сервиса
// (docs/service/README.md, «Контракт»).
//
// Принимается только грамматика RFC 8259: без комментариев, лишних
// запятых, NaN и ведущих нулей. Текст — корректный UTF-8, `\u` с
// суррогатами — только парами. Повторный ключ в объекте — ошибка.
// Вложенность ограничена: разбор рекурсивный, глубину задаёт вызывающий.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dp::json {

enum class Kind : uint8_t { Null, Bool, Number, String, Array, Object };

struct Value {
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;               // String — текст в UTF-8; Number — запись как во входе
    std::vector<Value> items;       // Array — элементы; Object — значения членов
    std::vector<std::string> keys;  // Object — ключи, keys[i] ↔ items[i], в порядке входа

    // Член объекта по ключу; nullptr — нет (или значение не объект).
    const Value* get(std::string_view key) const;
};

// Разбор text целиком. false — не JSON или вложенность глубже max_depth;
// error — что и где (по-английски, Р47: текст уходит пользователю).
bool parse(std::string_view text, uint32_t max_depth, Value& out, std::string& error);

}  // namespace dp::json
