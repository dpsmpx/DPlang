// Ключевые слова C (C11 и C23): имя dpl, которое C не примет как имя
// символа, параметра или поля заголовка (Р61; docs/spec/core.md,
// ffi.export).
#pragma once

#include <string_view>

namespace dp {

inline bool is_c_keyword(std::string_view s) {
    static constexpr std::string_view kKeywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
        "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return",
        "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while", "alignas", "alignof", "bool", "true", "false", "nullptr", "static_assert",
        "thread_local", "typeof", "typeof_unqual", "constexpr"};
    for (std::string_view k : kKeywords) {
        if (s == k) return true;
    }
    return false;
}

}  // namespace dp
