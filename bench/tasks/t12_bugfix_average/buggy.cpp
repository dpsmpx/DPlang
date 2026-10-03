// t12: реализация с ошибками — исправить, не переписывая заново.
#include <cstdint>
#include <cstdio>
#include <vector>

std::vector<int32_t> moving_average(const std::vector<int32_t>& values, int32_t window) {
    std::vector<int32_t> out;
    int32_t n = (int32_t)values.size();
    if (window > n) return out;
    int32_t sum = 0;
    for (int32_t i = 0; i < window; i++) sum += values[i];
    out.push_back(sum / window);
    for (int32_t i = window; i < n - 1; i++) {
        sum += values[i] - values[i - window];
        out.push_back(sum / window);
    }
    return out;
}

// --- driver ---
static void run(const std::vector<int32_t>& v, int32_t w) {
    auto r = moving_average(v, w);
    std::printf("w=%d n=%d:", w, (int)r.size());
    for (int32_t x : r) std::printf(" %d", x);
    std::printf("\n");
}

int main() {
    run({1, 2, 3, 4, 5}, 2);
    run({1, 2, 3, 4, 5}, 5);
    run({1, 2, 3, 4, 5}, 6);
    run({1, 2, 3}, 0);
    run({-5, -4, 7, 8}, 3);
    run({2147483647, 2147483647, 2147483647}, 2);
    run({}, 1);
    return 0;
}
