// t05: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <initializer_list>

uint32_t hash32(uint64_t seed, int32_t x, int32_t y) {
    uint64_t k = seed ^ (uint64_t(uint32_t(x)) << 32) ^ uint64_t(uint32_t(y));
    uint64_t z = k + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return uint32_t(z >> 32);
}

static int32_t raw_value(uint64_t seed, int32_t x, int32_t y) {
    return int32_t(32 + hash32(seed, x, y) % 16);
}

// Сдвиг координаты с переносом по модулю 2^32 (без переполнения знакового).
static int32_t shift(int32_t v, int32_t d) {
    return int32_t(uint32_t(v) + uint32_t(d));
}

int32_t height(uint64_t seed, int32_t x, int32_t y) {
    int32_t sum = raw_value(seed, x, y)
                + raw_value(seed, shift(x, 1), y)
                + raw_value(seed, shift(x, -1), y)
                + raw_value(seed, x, shift(y, 1))
                + raw_value(seed, x, shift(y, -1));
    return sum / 5;
}
