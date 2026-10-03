// Эталон t07 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

std::optional<int> shortest_path(const std::vector<std::string>& rows) {
    int h = (int)rows.size();
    int w = h ? (int)rows[0].size() : 0;
    int sx = -1, sy = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (rows[y][x] == 'S') { sx = x; sy = y; }
    if (sx < 0) return std::nullopt;
    std::vector<int> dist(w * h, -1);
    std::vector<int> queue;
    queue.push_back(sy * w + sx);
    dist[sy * w + sx] = 0;
    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (size_t head = 0; head < queue.size(); head++) {
        int cur = queue[head];
        int cx = cur % w, cy = cur / w;
        if (rows[cy][cx] == 'G') return dist[cur];
        for (int d = 0; d < 4; d++) {
            int nx = cx + dx[d], ny = cy + dy[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (rows[ny][nx] == '#' || dist[ny * w + nx] >= 0) continue;
            dist[ny * w + nx] = dist[cur] + 1;
            queue.push_back(ny * w + nx);
        }
    }
    return std::nullopt;
}

// --- driver ---
static void run(const std::vector<std::string>& rows) {
    auto r = shortest_path(rows);
    if (r) std::printf("path %d\n", *r); else std::printf("path none\n");
}

int main() {
    run({"S..G"});
    run({"S#.",
         ".#.",
         "..G"});
    run({"S.#.",
         ".##.",
         "...#",
         "#.#G"});
    run({"S.........",
         "########..",
         "..........",
         ".#########",
         "........G."});
    run({"SG"});
    run({"S#G"});
    return 0;
}
