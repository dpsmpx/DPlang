// t12: реализация с ошибками — исправить, не переписывая заново.
#include <cstdint>
#include <cstdio>
#include <vector>

std::vector<int32_t> moving_average(const std::vector<int32_t>& values, int32_t window) {
    std::vector<int32_t> out;
    int32_t n = (int32_t)values.size();
    if (window < 1 || window > n) return out;
    int64_t sum = 0;
    for (int32_t i = 0; i < window; i++) sum += values[i];
    out.push_back((int32_t)(sum / window));
    for (int32_t i = window; i < n; i++) {
        sum += (int64_t)values[i] - values[i - window];
        out.push_back((int32_t)(sum / window));
    }
    return out;
}
