// Эталон t06 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdio>
#include <vector>

struct Quad { int x, y, w, h, block; };

std::vector<Quad> greedy_mesh(int w, int h, const std::vector<int>& cells) {
    std::vector<bool> seen(cells.size(), false);
    std::vector<Quad> out;
    auto free_eq = [&](int x, int y, int b) { return cells[y * w + x] == b && !seen[y * w + x]; };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int b = cells[y * w + x];
            if (b == 0 || seen[y * w + x]) continue;
            int wd = 1;
            while (x + wd < w && free_eq(x + wd, y, b)) wd++;
            int ht = 1;
            for (;;) {
                if (y + ht >= h) break;
                bool row = true;
                for (int i = 0; i < wd; i++)
                    if (!free_eq(x + i, y + ht, b)) { row = false; break; }
                if (!row) break;
                ht++;
            }
            for (int j = 0; j < ht; j++)
                for (int i = 0; i < wd; i++) seen[(y + j) * w + x + i] = true;
            out.push_back({x, y, wd, ht, b});
        }
    }
    return out;
}

// --- driver ---
static void run(int w, int h, const std::vector<int>& cells) {
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
