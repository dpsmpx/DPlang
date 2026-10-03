// Драйвер t05 для переведённых вариантов DPlang (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

int main() {
    std::printf("%08x\n", hash32(0, 0, 0));
    std::printf("%08x\n", hash32(42, 1, 2));
    std::printf("%08x\n", hash32(42, -1, -2));
    std::printf("%08x\n", hash32(0xFFFFFFFFFFFFFFFFull, 2147483647, -2147483647 - 1));
    for (uint64_t seed : {1ull, 2026ull}) {
        std::printf("seed %llu\n", (unsigned long long)seed);
        for (int y = -2; y < 3; y++) {
            for (int x = -2; x < 3; x++) std::printf(x == -2 ? "%d" : " %d", height(seed, x, y));
            std::printf("\n");
        }
    }
    return 0;
}
