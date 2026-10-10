// t06: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <vector>

struct Quad { int x, y, w, h, block; };

std::vector<Quad> greedy_mesh(int w, int h, const std::vector<int>& cells) {
    std::vector<Quad> out;
    if (w <= 0 || h <= 0) return out;
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    auto at = [&](int x, int y) -> int {
        size_t i = static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x);
        return i < cells.size() ? cells[i] : 0;
    };
    std::vector<char> visited(total, 0);
    auto vis = [&](int x, int y) -> char& {
        return visited[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int b = at(x, y);
            if (b == 0 || vis(x, y)) continue;
            int wd = 0;
            while (x + wd < w && at(x + wd, y) == b && !vis(x + wd, y)) ++wd;
            int ht = 0;
            while (y + ht < h) {
                bool ok = true;
                for (int i = 0; i < wd; ++i) {
                    if (at(x + i, y + ht) != b || vis(x + i, y + ht)) { ok = false; break; }
                }
                if (!ok) break;
                ++ht;
            }
            for (int dy = 0; dy < ht; ++dy)
                for (int dx = 0; dx < wd; ++dx) vis(x + dx, y + dy) = 1;
            out.push_back(Quad{x, y, wd, ht, b});
        }
    }
    return out;
}
