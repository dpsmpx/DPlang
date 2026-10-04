// t07: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

std::optional<int> shortest_path(const std::vector<std::string>& rows) {
    const int h = static_cast<int>(rows.size());
    if (h == 0) return std::nullopt;

    // Ширина — по самой длинной строке; клетки за концом короткой строки — стены.
    int w = 0;
    for (const auto& r : rows) {
        if (static_cast<int>(r.size()) > w) w = static_cast<int>(r.size());
    }
    if (w == 0) return std::nullopt;

    auto cell = [&](int y, int x) -> char {
        const std::string& r = rows[static_cast<size_t>(y)];
        if (x < 0 || static_cast<size_t>(x) >= r.size()) return '#';
        return r[static_cast<size_t>(x)];
    };

    int sy = -1, sx = -1;
    bool has_goal = false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const char c = cell(y, x);
            if (c == 'S' && sy < 0) {
                sy = y;
                sx = x;
            } else if (c == 'G') {
                has_goal = true;
            }
        }
    }
    if (sy < 0 || !has_goal) return std::nullopt;

    const size_t total = static_cast<size_t>(h) * static_cast<size_t>(w);
    std::vector<int> dist(total, -1);
    std::vector<int> queue;
    queue.reserve(total);

    const size_t start = static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx);
    dist[start] = 0;
    queue.push_back(static_cast<int>(start));

    static const int dy[4] = {-1, 1, 0, 0};
    static const int dx[4] = {0, 0, -1, 1};

    for (size_t head = 0; head < queue.size(); ++head) {
        const int idx = queue[head];
        const int y = idx / w;
        const int x = idx % w;
        const int d = dist[static_cast<size_t>(idx)];
        if (cell(y, x) == 'G') return d;
        for (int k = 0; k < 4; ++k) {
            const int ny = y + dy[k];
            const int nx = x + dx[k];
            if (ny < 0 || ny >= h || nx < 0 || nx >= w) continue;
            const char c = cell(ny, nx);
            if (c == '#') continue;
            const size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx);
            if (dist[nidx] >= 0) continue;
            dist[nidx] = d + 1;
            queue.push_back(static_cast<int>(nidx));
        }
    }
    return std::nullopt;
}
