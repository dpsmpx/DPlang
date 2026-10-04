// UTF-8: разбор, запись, очистка для вывода.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dp {

// Длина корректной последовательности UTF-8 с text[i]; 0 — последовательность
// неверна (обрезана, избыточна, суррогат, больше U+10FFFF). cp — код символа.
inline uint32_t decode_utf8(std::string_view text, size_t i, uint32_t& cp) {
    unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    uint32_t len, min;
    if ((c & 0xE0) == 0xC0) {
        len = 2, min = 0x80, cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3, min = 0x800, cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4, min = 0x10000, cp = c & 0x07;
    } else {
        return 0;
    }
    if (i + len > text.size()) return 0;
    for (uint32_t k = 1; k < len; k++) {
        unsigned char d = static_cast<unsigned char>(text[i + k]);
        if ((d & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (d & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

inline void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Текст для вывода: каждый неверный байт заменён на U+FFFD — вывод
// компилятора всегда корректный UTF-8.
inline std::string sanitize_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        uint32_t cp;
        uint32_t n = decode_utf8(text, i, cp);
        if (n == 0) {
            out += "\xEF\xBF\xBD";
            i++;
        } else {
            out.append(text.substr(i, n));
            i += n;
        }
    }
    return out;
}

}  // namespace dp
