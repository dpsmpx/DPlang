// Эталон t08 на C++20. Над чертой — решение, под чертой — драйвер.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Box { int min_x, min_y, max_x, max_y; };
struct Pair { int i, j; };

std::vector<Pair> overlapping_pairs(const std::vector<Box>& boxes) {
    int n = (int)boxes.size();
    std::vector<int> order(n);
    for (int i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (boxes[a].min_x != boxes[b].min_x) return boxes[a].min_x < boxes[b].min_x;
        return a < b;
    });
    std::vector<Pair> out;
    for (int k = 0; k < n; k++) {
        const Box& a = boxes[order[k]];
        for (int m = k + 1; m < n; m++) {
            const Box& b = boxes[order[m]];
            if (b.min_x >= a.max_x) break;
            if (a.min_y < b.max_y && b.min_y < a.max_y) {
                int i = std::min(order[k], order[m]), j = std::max(order[k], order[m]);
                out.push_back({i, j});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](Pair p, Pair q) { return p.i != q.i ? p.i < q.i : p.j < q.j; });
    return out;
}

// --- driver ---
static void run(const std::vector<Box>& boxes, bool print_pairs) {
    auto pairs = overlapping_pairs(boxes);
    std::printf("pairs %d\n", (int)pairs.size());
    if (print_pairs)
        for (Pair p : pairs) std::printf("  %d %d\n", p.i, p.j);
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
    auto pairs = overlapping_pairs(many);
    long long sum = 0;
    for (Pair p : pairs) sum += (long long)p.i * 1000 + p.j;
    std::printf("pairs %d checksum %lld\n", (int)pairs.size(), sum);
    return 0;
}
