// Драйвер t08 для переведённых вариантов DPlang (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void run(const std::vector<Box>& boxes, bool print_pairs) {
    std::vector<Pair> pairs = overlapping_pairs(boxes);
    std::printf("pairs %d\n", (int)pairs.size());
    if (print_pairs)
        for (const Pair& p : pairs) std::printf("  %d %d\n", p.i, p.j);
}

int main() {
    run({{0, 0, 2, 2}, {1, 1, 3, 3}, {2, 0, 4, 2}, {5, 5, 6, 6}}, true);
    run({{0, 0, 10, 10}, {1, 1, 2, 2}, {8, 8, 9, 9}, {10, 0, 11, 1}}, true);
    run({}, true);
    std::vector<Box> many;
    uint32_t s = 12345;
    auto next = [&]() { s = s * 1103515245u + 12345u; return (int)((s >> 16) & 0x7fff); };
    for (int i = 0; i < 200; i++) {
        int x = next() % 1000, y = next() % 1000;
        int w = 1 + next() % 60, h = 1 + next() % 60;
        many.push_back({x, y, x + w, y + h});
    }
    std::vector<Pair> pairs = overlapping_pairs(many);
    long long sum = 0;
    for (const Pair& p : pairs) sum += (long long)p.i * 1000 + p.j;
    std::printf("pairs %d checksum %lld\n", (int)pairs.size(), sum);
    return 0;
}
