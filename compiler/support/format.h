// Запись чисел строкой — одна для свёртки констант, интерпретатора и
// runtime (docs/spec/core.md, expr.convert).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dp {

// Кратчайшая десятичная запись, которая читается обратно в то же число
// (для f32 — в то же f32), как repr в Python, но без хвоста `.0`
// (Р48): обычная форма при десятичном порядке от −4 до 15 — `0.1`,
// `100`, `0.0001`; иначе экспоненциальная — `1e+16`, `1.5e-07`. Ноль —
// `0` и `-0`, бесконечности — `inf` и `-inf`, нечисло — `nan`.
inline std::string format_float(double v, bool f32) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0) return std::signbit(v) ? "-0" : "0";
    char buf[48];
    for (int p = 1; p <= 17; p++) {
        std::snprintf(buf, sizeof buf, "%.*e", p - 1, v);
        double back = std::strtod(buf, nullptr);
        if (f32 ? static_cast<float>(back) == static_cast<float>(v) : back == v) break;
    }
    // buf: [-]d[.ddd]e±XX — цифры и десятичный порядок.
    std::string text = buf;
    bool neg = text[0] == '-';
    size_t e = text.find('e');
    std::string digits;
    for (size_t i = neg ? 1 : 0; i < e; i++)
        if (text[i] != '.') digits += text[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    int exp = std::atoi(text.c_str() + e + 1);
    std::string out = neg ? "-" : "";
    if (exp < -4 || exp >= 16) {
        out += digits[0];
        if (digits.size() > 1) out += "." + digits.substr(1);
        char ebuf[16];
        std::snprintf(ebuf, sizeof ebuf, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        return out + ebuf;
    }
    if (exp < 0) return out + "0." + std::string(static_cast<size_t>(-exp - 1), '0') + digits;
    size_t int_len = static_cast<size_t>(exp) + 1;
    if (digits.size() <= int_len) return out + digits + std::string(int_len - digits.size(), '0');
    return out + digits.substr(0, int_len) + "." + digits.substr(int_len);
}

}  // namespace dp
