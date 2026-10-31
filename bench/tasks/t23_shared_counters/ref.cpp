// Эталон t23 на C++20. Над чертой — решение, под чертой — драйвер.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int64_t cost(int64_t v) {
    uint64_t h = (uint64_t)v;
    for (int32_t i = 0; i < 2000; i++) h = mix(h);
    return (int64_t)(h % 1000);
}

struct Totals {
    bool ok = true;
    int64_t bad = -1, count = 0, sum = 0, odd = 0;
};

Totals run_jobs(const std::vector<int64_t>& jobs, int32_t workers) {
    int64_t n = (int64_t)jobs.size();
    std::atomic<int64_t> next{0}, count{0}, sum{0}, odd{0}, bad{n};
    std::atomic<bool> stop{false};
    std::vector<std::thread> pool;
    for (int32_t w = 0; w < workers; w++) {
        pool.emplace_back([&] {
            while (!stop.load()) {
                int64_t j = next.fetch_add(1);
                if (j >= n) break;
                if (jobs[j] < 0) {
                    stop.store(true);
                    int64_t cur = bad.load();
                    while (j < cur && !bad.compare_exchange_weak(cur, j)) {
                    }
                    continue;
                }
                int64_t r = cost(jobs[j]);
                count.fetch_add(1);
                sum.fetch_add(r);
                if (r % 2 != 0) odd.fetch_add(1);
            }
        });
    }
    for (std::thread& th : pool) th.join();
    Totals t;
    if (bad.load() < n) {
        t.ok = false;
        t.bad = bad.load();
        return t;
    }
    t.count = count.load();
    t.sum = sum.load();
    t.odd = odd.load();
    return t;
}

// --- driver ---
static void print(const char* name, const Totals& t) {
    std::printf("%s ok=%s bad=%lld count=%lld sum=%lld odd=%lld\n", name, t.ok ? "true" : "false", (long long)t.bad,
                (long long)t.count, (long long)t.sum, (long long)t.odd);
}

int main() {
    std::vector<int64_t> jobs;
    for (uint64_t i = 0; i < 20000; i++) jobs.push_back((int64_t)(mix(i) >> 2));
    for (int32_t w : {1, 4, 8}) {
        char name[16];
        std::snprintf(name, sizeof name, "w=%d", w);
        print(name, run_jobs(jobs, w));
    }
    jobs[15000] = -5;
    jobs[12345] = -1;
    for (int32_t w : {1, 3, 8}) {
        char name[16];
        std::snprintf(name, sizeof name, "bad w=%d", w);
        print(name, run_jobs(jobs, w));
    }
    print("empty", run_jobs({}, 2));
    print("one", run_jobs({-1}, 3));
    print("small", run_jobs({5, -2, 7, -3}, 2));
    print("tiny", run_jobs({1, 2, 3}, 5));
    return 0;
}
