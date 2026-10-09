// Запись чисел строкой — одна для свёртки констант, интерпретатора и
// runtime (docs/spec/core.md, expr.convert; runtime/core/dp_rt.c —
// тот же алгоритм на C).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dp {

namespace format_detail {

// Десятичное d·10^k читается обратно в v (у f32 — в то же f32, без
// двойного округления через double).
inline bool reads_back(uint64_t d, int k, double v, bool f32) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%llue%d", static_cast<unsigned long long>(d), k);
    if (f32) return std::strtof(buf, nullptr) == static_cast<float>(v);
    return std::strtod(buf, nullptr) == v;
}

}  // namespace format_detail

// Кратчайшая десятичная запись, которая читается обратно в то же число
// (для f32 — в то же f32), как repr в Python, но без хвоста `.0`
// (Р48): обычная форма при десятичном порядке от −4 до 15 — `0.1`,
// `100`, `0.0001`; иначе экспоненциальная — `1e+16`, `1.5e-07`. Ноль —
// `0` и `-0`, бесконечности — `inf` и `-inf`, нечисло — `nan`.
//
// Для каждой длины p — от 1 до 17 цифр — проверяются правильно
// округлённая p-значная запись и её соседи на одну единицу последнего
// разряда. Сосед нужен у степеней двойки: там промежуток округления
// несимметричен, и ближайшая p-значная запись может не читаться обратно,
// хотя соседняя читается. Из подходящих берётся ближайшая к v.
inline std::string format_float(double v, bool f32) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0) return std::signbit(v) ? "-0" : "0";
    bool neg = v < 0;
    double a = std::fabs(v);
    uint64_t d = 0;
    int k = 0;
    for (int p = 1; p <= 17; p++) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        // buf: d[.ddd]e±XX — p цифр и десятичный порядок первой.
        d = 0;
        const char* c = buf;
        for (; *c != 'e'; c++) {
            if (*c != '.') d = d * 10 + static_cast<uint64_t>(*c - '0');
        }
        k = std::atoi(c + 1) - (p - 1);
        if (format_detail::reads_back(d, k, a, f32)) break;
        uint64_t low = 1;
        for (int i = 1; i < p; i++) low *= 10;  // 10^(p-1): у 1.00…0 сосед снизу — 9.99…9 порядком ниже
        uint64_t below = d == low ? d * 10 - 1 : d - 1;
        int below_k = d == low ? k - 1 : k;
        if (format_detail::reads_back(below, below_k, a, f32)) {
            d = below;
            k = below_k;
            break;
        }
        if (format_detail::reads_back(d + 1, k, a, f32)) {
            d = d + 1;
            break;
        }
    }
    std::string digits = std::to_string(d);
    while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
        k++;
    }
    int exp = k + static_cast<int>(digits.size()) - 1;  // десятичный порядок первой цифры
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
