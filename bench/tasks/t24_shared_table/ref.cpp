// Эталон t24 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

struct Move {
    int32_t from, to;
    int64_t amount;
};

struct Report {
    std::vector<int64_t> table;
    int64_t reads = 0, bad = 0;
};

Report run_bank(int32_t accounts, const std::vector<Move>& moves, int32_t readers, int32_t reads) {
    std::vector<int64_t> table(accounts, 1000);
    std::shared_mutex m;
    const int64_t total = (int64_t)accounts * 1000;
    std::vector<int64_t> bad(readers, 0);
    std::vector<std::thread> pool;
    pool.emplace_back([&] {
        for (const Move& mv : moves) {
            std::unique_lock<std::shared_mutex> lk(m);
            int64_t a = mv.amount < table[mv.from] ? mv.amount : table[mv.from];
            table[mv.from] -= a;
            table[mv.to] += a;
        }
    });
    for (int32_t r = 0; r < readers; r++) {
        pool.emplace_back([&, r] {
            for (int32_t k = 0; k < reads; k++) {
                std::shared_lock<std::shared_mutex> lk(m);
                int64_t s = 0;
                for (int64_t v : table) s += v;
                if (s != total) bad[r]++;
            }
        });
    }
    for (std::thread& th : pool) th.join();
    Report rep;
    rep.table = table;
    rep.reads = (int64_t)readers * reads;
    for (int64_t b : bad) rep.bad += b;
    return rep;
}

// --- driver ---
static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void print(const char* name, const Report& r) {
    int64_t check = 0;
    for (size_t i = 0; i < r.table.size(); i++) check += (int64_t)(i + 1) * r.table[i];
    std::printf("%s check=%lld first=%lld last=%lld reads=%lld bad=%lld\n", name, (long long)check,
                (long long)(r.table.empty() ? 0 : r.table.front()), (long long)(r.table.empty() ? 0 : r.table.back()),
                (long long)r.reads, (long long)r.bad);
}

int main() {
    const int32_t accounts = 64;
    std::vector<Move> moves;
    for (uint64_t i = 0; i < 200000; i++) {
        uint64_t h = mix(i);
        moves.push_back(Move{(int32_t)(h % accounts), (int32_t)((h >> 8) % accounts), (int64_t)((h >> 16) % 1500)});
    }
    print("r=1", run_bank(accounts, moves, 1, 3000));
    print("r=4", run_bank(accounts, moves, 4, 3000));
    std::vector<Move> few{{0, 1, 300}, {1, 2, 5000}, {2, 2, 7}, {2, 0, 1}};
    Report s = run_bank(3, few, 2, 10);
    std::printf("small %lld %lld %lld reads=%lld bad=%lld\n", (long long)s.table[0], (long long)s.table[1],
                (long long)s.table[2], (long long)s.reads, (long long)s.bad);
    print("none", run_bank(5, {}, 3, 0));
    return 0;
}
