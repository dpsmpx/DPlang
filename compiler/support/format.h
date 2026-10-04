// Запись чисел строкой — одна для свёртки констант, интерпретатора и
// runtime (docs/spec/core.md, expr.convert).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dp {

// Кратчайшая десятичная запись, которая читается обратно в то же число
// (для f32 — в то же f32): `0.1`, `1e+20`, `-0`. Бесконечности — `inf` и
// `-inf`, нечисло — `nan`.
inline std::string format_float(double v, bool f32) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    char buf[40];
    for (int p = 1; p <= 17; p++) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        double back = std::strtod(buf, nullptr);
        if (f32 ? static_cast<float>(back) == static_cast<float>(v) : back == v) break;
    }
    return buf;
}

}  // namespace dp
