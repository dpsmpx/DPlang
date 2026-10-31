// Эталон t20 на C++20. Над чертой — решение, под чертой — драйвер.
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int64_t build(int64_t chunk, uint64_t seed) {
    uint64_t h = (uint64_t)chunk ^ seed;
    for (int32_t i = 0; i < 20000; i++) h = mix(h);
    return (int64_t)(h % 100000);
}

struct Cmd {
    int32_t kind;
    int64_t chunk;
    uint64_t seed;
};

// Готовые результаты: общие для менеджера и сборок; сборка держит их и
// после ухода менеджера.
struct Ready {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<int64_t, int64_t>> done;  // (номер заказа, значение)
};

std::vector<std::string> run(const std::vector<Cmd>& script, int32_t limit) {
    auto ready = std::make_shared<Ready>();
    std::map<int64_t, int64_t> state, latest;  // чанк → значение; чанк → номер последнего заказа
    std::map<int64_t, int64_t> chunk_of;       // номер заказа → чанк
    std::vector<std::string> out;
    int64_t orders = 0, taken = 0, syncs = 0;
    auto apply = [&](std::pair<int64_t, int64_t> r) {
        int64_t c = chunk_of[r.first];
        if (latest[c] == r.first) state[c] = r.second;
        taken++;
    };
    for (const Cmd& c : script) {
        if (c.kind == 0) {
            int64_t id = ++orders;
            latest[c.chunk] = id;
            chunk_of[id] = c.chunk;
            std::thread([ready, id, c] {
                int64_t v = build(c.chunk, c.seed);
                std::lock_guard<std::mutex> lk(ready->m);
                ready->done.push_back({id, v});
                ready->cv.notify_all();
            }).detach();
        } else if (c.kind == 1) {
            std::vector<std::pair<int64_t, int64_t>> got;
            {
                std::lock_guard<std::mutex> lk(ready->m);
                while (!ready->done.empty() && (int32_t)got.size() < limit) {
                    got.push_back(ready->done.front());
                    ready->done.pop_front();
                }
            }
            for (auto& r : got) apply(r);
        } else if (c.kind == 2) {
            while (taken < orders) {
                std::pair<int64_t, int64_t> r;
                {
                    std::unique_lock<std::mutex> lk(ready->m);
                    ready->cv.wait(lk, [&] { return !ready->done.empty(); });
                    r = ready->done.front();
                    ready->done.pop_front();
                }
                apply(r);
            }
            std::string line = "sync " + std::to_string(++syncs) + ":";
            for (auto& [k, v] : state) line += " " + std::to_string(k) + "=" + std::to_string(v);
            out.push_back(line);
        } else {
            break;
        }
    }
    return out;
}

// --- driver ---
int main() {
    std::vector<Cmd> script;
    for (uint64_t i = 0; i < 300; i++) {
        uint64_t h = mix(i + 1000);
        script.push_back(Cmd{0, (int64_t)(h % 23), h >> 20});
        if (i % 3 == 0) script.push_back(Cmd{1, 0, 0});
        if (i % 100 == 99) script.push_back(Cmd{2, 0, 0});
    }
    script.push_back(Cmd{1, 0, 0});
    for (uint64_t i = 0; i < 40; i++) script.push_back(Cmd{0, (int64_t)i, mix(i)});
    script.push_back(Cmd{3, 0, 0});
    script.push_back(Cmd{2, 0, 0});
    for (const std::string& s : run(script, 2)) std::printf("%s\n", s.c_str());
    std::vector<Cmd> small{{0, 5, 1}, {0, 5, 2}, {1, 0, 0}, {0, -3, 9}, {2, 0, 0}, {2, 0, 0}};
    for (const std::string& s : run(small, 1)) std::printf("small %s\n", s.c_str());
    std::printf("empty %zu\n", run({}, 3).size());
    std::vector<Cmd> only{{2, 0, 0}};
    for (const std::string& s : run(only, 1)) std::printf("only %s\n", s.c_str());
    return 0;
}
