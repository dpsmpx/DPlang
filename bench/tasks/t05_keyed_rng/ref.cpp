// Эталон t05 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdint>
#include <cstdio>
#include <initializer_list>

uint32_t hash32(uint64_t seed, int32_t x, int32_t y) {
    uint64_t k = seed ^ ((uint64_t)(uint32_t)x << 32) ^ (uint64_t)(uint32_t)y;
    uint64_t z = k + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return (uint32_t)(z >> 32);
}

static int32_t raw(uint64_t seed, int32_t x, int32_t y) {
    return 32 + (int32_t)(hash32(seed, x, y) % 16);
}

int32_t height(uint64_t seed, int32_t x, int32_t y) {
    int32_t sum = raw(seed, x, y) + raw(seed, x - 1, y) + raw(seed, x + 1, y) +
                  raw(seed, x, y - 1) + raw(seed, x, y + 1);
    return sum / 5;
}

// --- driver ---
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
