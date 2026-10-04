#include "syntax/literal.h"

#include <cmath>
#include <cstdlib>

namespace dp {

namespace {

int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

bool is_digit_of(char c, int base) { return digit_value(c) < base; }

// Цифры с `_` только между цифрами: непусто, без `_` в начале, в конце и подряд.
bool digits_ok(std::string_view s, int base) {
    if (s.empty() || s.front() == '_' || s.back() == '_') return false;
    char prev = 0;
    for (char c : s) {
        if (c == '_') {
            if (prev == '_') return false;
        } else if (!is_digit_of(c, base)) {
            return false;
        }
        prev = c;
    }
    return true;
}

}  // namespace

LitStatus parse_int_literal(std::string_view text, uint64_t& value) {
    int base = 10;
    std::string_view digits = text;
    if (text.size() >= 2 && text[0] == '0') {
        char p = text[1];
        if (p == 'x' || p == 'X') base = 16;
        else if (p == 'b' || p == 'B') base = 2;
        else if (p == 'o' || p == 'O') base = 8;
        if (base != 10) digits = text.substr(2);
    }
    if (!digits_ok(digits, base)) return LitStatus::Malformed;
    uint64_t v = 0;
    for (char c : digits) {
        if (c == '_') continue;
        uint64_t d = static_cast<uint64_t>(digit_value(c));
        if (v > (UINT64_MAX - d) / static_cast<uint64_t>(base)) return LitStatus::OutOfRange;
        v = v * static_cast<uint64_t>(base) + d;
    }
    value = v;
    return LitStatus::Ok;
}

LitStatus parse_float_literal(std::string_view text, double& value) {
    // целая часть [. дробная] [e[+-]порядок]
    size_t i = 0;
    while (i < text.size() && (is_digit_of(text[i], 10) || text[i] == '_')) i++;
    if (!digits_ok(text.substr(0, i), 10)) return LitStatus::Malformed;
    if (i < text.size() && text[i] == '.') {
        size_t j = ++i;
        while (i < text.size() && (is_digit_of(text[i], 10) || text[i] == '_')) i++;
        if (!digits_ok(text.substr(j, i - j), 10)) return LitStatus::Malformed;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        i++;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
        size_t j = i;
        while (i < text.size() && (is_digit_of(text[i], 10) || text[i] == '_')) i++;
        if (!digits_ok(text.substr(j, i - j), 10)) return LitStatus::Malformed;
    }
    if (i != text.size()) return LitStatus::Malformed;
    std::string clean;
    for (char c : text) {
        if (c != '_') clean += c;
    }
    char* end = nullptr;
    double v = std::strtod(clean.c_str(), &end);
    if (end != clean.c_str() + clean.size()) return LitStatus::Malformed;
    if (!std::isfinite(v)) return LitStatus::OutOfRange;
    value = v;
    return LitStatus::Ok;
}

Escape parse_escape(std::string_view text, size_t i) {
    Escape e;
    if (i + 1 >= text.size()) {
        e.bad_len = 1;
        return e;
    }
    char c = text[i + 1];
    switch (c) {
        case 'n': e.value = '\n'; e.len = 2; return e;
        case 't': e.value = '\t'; e.len = 2; return e;
        case 'r': e.value = '\r'; e.len = 2; return e;
        case '0': e.value = 0; e.len = 2; return e;
        case '\\': e.value = '\\'; e.len = 2; return e;
        case '"': e.value = '"'; e.len = 2; return e;
        case '\'': e.value = '\''; e.len = 2; return e;
        case 'x': {
            if (i + 3 < text.size() && is_digit_of(text[i + 2], 16) && is_digit_of(text[i + 3], 16)) {
                e.value = static_cast<uint32_t>(digit_value(text[i + 2]) * 16 + digit_value(text[i + 3]));
                e.len = 4;
                return e;
            }
            e.bad_len = 2;
            return e;
        }
        case 'u': {
            size_t j = i + 2;
            if (j >= text.size() || text[j] != '{') {
                e.bad_len = 2;
                return e;
            }
            j++;
            uint32_t v = 0;
            size_t n = 0;
            while (j < text.size() && is_digit_of(text[j], 16) && n < 7) {
                v = v * 16 + static_cast<uint32_t>(digit_value(text[j]));
                j++;
                n++;
            }
            bool closed = j < text.size() && text[j] == '}';
            if (!closed || n == 0 || n > 6 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
                e.bad_len = static_cast<uint32_t>((closed ? j + 1 : j) - i);
                return e;
            }
            e.value = v;
            e.is_unicode = true;
            e.len = static_cast<uint32_t>(j + 1 - i);
            return e;
        }
        default:
            e.bad_len = 2;
            return e;
    }
}

std::string string_literal_value(std::string_view text) {
    std::string out;
    // text — с кавычками; закрывающей может не быть у ошибочного литерала.
    size_t end = text.size() >= 2 && text.back() == '"' ? text.size() - 1 : text.size();
    for (size_t i = 1; i < end;) {
        if (text[i] == '\\') {
            Escape e = parse_escape(text, i);
            if (e.len == 0) {
                i += e.bad_len ? e.bad_len : 1;
                continue;
            }
            if (e.is_unicode) append_utf8(out, e.value);
            else out += static_cast<char>(e.value);
            i += e.len;
        } else {
            out += text[i++];
        }
    }
    return out;
}

uint8_t byte_literal_value(std::string_view text) {
    if (text.size() >= 3 && text[1] == '\\') {
        Escape e = parse_escape(text, 1);
        return static_cast<uint8_t>(e.value);
    }
    return text.size() >= 2 ? static_cast<uint8_t>(text[1]) : 0;
}

}  // namespace dp
