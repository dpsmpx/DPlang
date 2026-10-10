// t07: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

std::optional<int> shortest_path(const std::vector<std::string>& rows) {
    const int h = static_cast<int>(rows.size());
    if (h == 0) {
        return std::nullopt;
    }
    int sr = -1, sc = -1, gr = -1, gc = -1;
    for (int r = 0; r < h; ++r) {
        const int w = static_cast<int>(rows[r].size());
        for (int c = 0; c < w; ++c) {
            const char ch = rows[r][c];
            if (ch == 'S') {
                sr = r;
                sc = c;
            } else if (ch == 'G') {
                gr = r;
                gc = c;
            }
        }
    }
    if (sr < 0 || gr < 0) {
        return std::nullopt;
    }
    if (sr == gr && sc == gc) {
        return 0;
    }

    std::vector<std::vector<int>> dist(static_cast<std::size_t>(h));
    for (int r = 0; r < h; ++r) {
        dist[static_cast<std::size_t>(r)].assign(rows[r].size(), -1);
    }

    std::vector<std::pair<int, int>> queue;
    queue.reserve(64);
    queue.emplace_back(sr, sc);
    dist[static_cast<std::size_t>(sr)][static_cast<std::size_t>(sc)] = 0;

    const int dr[4] = {-1, 1, 0, 0};
    const int dc[4] = {0, 0, -1, 1};

    std::size_t head = 0;
    while (head < queue.size()) {
        const auto [r, c] = queue[head++];
        const int d = dist[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
        for (int k = 0; k < 4; ++k) {
            const int nr = r + dr[k];
            const int nc = c + dc[k];
            if (nr < 0 || nr >= h) {
                continue;
            }
            const std::string& row = rows[static_cast<std::size_t>(nr)];
            if (nc < 0 || nc >= static_cast<int>(row.size())) {
                continue;
            }
            if (row[static_cast<std::size_t>(nc)] == '#') {
                continue;
            }
            int& nd = dist[static_cast<std::size_t>(nr)][static_cast<std::size_t>(nc)];
            if (nd != -1) {
                continue;
            }
            nd = d + 1;
            if (nr == gr && nc == gc) {
                return nd;
            }
            queue.emplace_back(nr, nc);
        }
    }
    return std::nullopt;
}
