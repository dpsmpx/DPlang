// Встроенные типы и функции (docs/spec/core.md, разделы 4 и 9).
#pragma once

#include <cstdint>
#include <string_view>

#include "sema/types.h"

namespace dp::sema {

struct BuiltinType {
    std::string_view name;
    TK kind;  // List — обобщённый
};

inline constexpr BuiltinType kBuiltinTypes[] = {
    {"bool", TK::Bool}, {"i8", TK::I8},   {"i16", TK::I16}, {"i32", TK::I32}, {"i64", TK::I64},
    {"int", TK::Int},   {"u8", TK::U8},   {"u16", TK::U16}, {"u32", TK::U32}, {"u64", TK::U64},
    {"f32", TK::F32},   {"f64", TK::F64}, {"str", TK::Str}, {"void", TK::Void}, {"List", TK::List},
};

enum class BuiltinFn : uint8_t {
    Len, Repeat, Min, Max, Abs, ParseInt, IsDigit, IsAlpha, IsSpace, IsAlnum, Assert, Print, Println, Eprintln,
    Count
};

inline constexpr std::string_view kBuiltinFnNames[] = {
    "len", "repeat", "min", "max", "abs", "parse_int", "is_digit", "is_alpha", "is_space", "is_alnum", "assert",
    "print", "println", "eprintln",
};

// Номер встроенного типа или функции по имени; -1 — нет такого.
inline int find_builtin_type(std::string_view name) {
    for (size_t i = 0; i < sizeof kBuiltinTypes / sizeof kBuiltinTypes[0]; i++) {
        if (kBuiltinTypes[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

inline int find_builtin_fn(std::string_view name) {
    for (size_t i = 0; i < static_cast<size_t>(BuiltinFn::Count); i++) {
        if (kBuiltinFnNames[i] == name) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace dp::sema
