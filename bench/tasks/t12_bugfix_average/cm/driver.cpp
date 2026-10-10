// Драйвер t12 для переведённых вариантов dpl (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void run(const std::vector<int32_t>& v, int32_t w) {
    std::vector<int32_t> r = moving_average(v, w);
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
