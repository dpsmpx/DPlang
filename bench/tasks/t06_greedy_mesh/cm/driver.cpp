// Драйвер t06 для переведённых вариантов C- (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void run(int32_t w, int32_t h, const std::vector<int32_t>& cells) {
    std::vector<Quad> q = greedy_mesh(w, h, cells);
    std::printf("quads %d\n", (int)q.size());
    for (const Quad& r : q) std::printf("  %d %d %d %d %d\n", r.x, r.y, r.w, r.h, r.block);
}

int main() {
    run(4, 3, {1, 1, 1, 1,
               1, 1, 1, 1,
               1, 1, 1, 1});
    run(4, 4, {1, 1, 2, 2,
               1, 1, 2, 2,
               0, 3, 3, 0,
               0, 3, 3, 0});
    run(5, 3, {1, 1, 1, 0, 1,
               1, 2, 1, 0, 1,
               1, 1, 1, 1, 1});
    run(3, 3, {0, 0, 0, 0, 0, 0, 0, 0, 0});
    run(6, 2, {4, 4, 4, 5, 5, 5,
               4, 4, 5, 5, 5, 5});
    return 0;
}
