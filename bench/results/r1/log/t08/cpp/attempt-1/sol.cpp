// t08: заготовка — напишите тела функций по task.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

struct Box { int min_x, min_y, max_x, max_y; };

struct Pair { int i, j; };

// Проход по оси x: индексы сортируются по min_x, для каждого
// прямоугольника просматриваются следующие, пока их min_x < max_x
// текущего. Пары затем упорядочиваются по (i, j).
std::vector<Pair> overlapping_pairs(const std::vector<Box>& boxes) {
    const int n = static_cast<int>(boxes.size());
    std::vector<int> order(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) order[static_cast<size_t>(k)] = k;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const Box& ba = boxes[static_cast<size_t>(a)];
        const Box& bb = boxes[static_cast<size_t>(b)];
        if (ba.min_x != bb.min_x) return ba.min_x < bb.min_x;
        return a < b;
    });

    std::vector<Pair> out;
    for (int p = 0; p < n; ++p) {
        const int ia = order[static_cast<size_t>(p)];
        const Box& a = boxes[static_cast<size_t>(ia)];
        for (int q = p + 1; q < n; ++q) {
            const int ib = order[static_cast<size_t>(q)];
            const Box& b = boxes[static_cast<size_t>(ib)];
            if (!(b.min_x < a.max_x)) break;
            // a.min_x <= b.min_x < b.max_x, значит a.min_x < b.max_x.
            if (a.min_y < b.max_y && b.min_y < a.max_y) {
                if (ia < ib) out.push_back(Pair{ia, ib});
                else out.push_back(Pair{ib, ia});
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const Pair& x, const Pair& y) {
        if (x.i != y.i) return x.i < y.i;
        return x.j < y.j;
    });
    return out;
}
