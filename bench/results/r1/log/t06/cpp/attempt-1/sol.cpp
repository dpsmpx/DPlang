// t06: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <vector>

struct Quad { int x, y, w, h, block; };

std::vector<Quad> greedy_mesh(int w, int h, const std::vector<int>& cells) {
    std::vector<Quad> out;
    if (w <= 0 || h <= 0) return out;
    const size_t W = static_cast<size_t>(w);
    const size_t H = static_cast<size_t>(h);
    if (cells.size() < W * H) return out;
    std::vector<char> visited(W * H, 0);
    auto free_with = [&](size_t x, size_t y, int b) {
        size_t i = y * W + x;
        return !visited[i] && cells[i] == b;
    };
    for (size_t y = 0; y < H; ++y) {
        for (size_t x = 0; x < W; ++x) {
            size_t i = y * W + x;
            int b = cells[i];
            if (b == 0 || visited[i]) continue;
            size_t wd = 1;
            while (x + wd < W && free_with(x + wd, y, b)) ++wd;
            size_t ht = 1;
            while (y + ht < H) {
                bool ok = true;
                for (size_t k = 0; k < wd; ++k) {
                    if (!free_with(x + k, y + ht, b)) { ok = false; break; }
                }
                if (!ok) break;
                ++ht;
            }
            for (size_t dy = 0; dy < ht; ++dy)
                for (size_t dx = 0; dx < wd; ++dx)
                    visited[(y + dy) * W + (x + dx)] = 1;
            out.push_back(Quad{static_cast<int>(x), static_cast<int>(y),
                               static_cast<int>(wd), static_cast<int>(ht), b});
        }
    }
    return out;
}
