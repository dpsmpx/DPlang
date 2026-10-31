// t23: заготовка — напишите тело run_jobs по task.md.
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

namespace {

void store_min(std::atomic<int64_t>& target, int64_t v) {
    int64_t cur = target.load();
    while ((cur < 0 || v < cur) && !target.compare_exchange_weak(cur, v)) {
    }
}

}  // namespace

Totals run_jobs(const std::vector<int64_t>& jobs, int32_t workers) {
    const int64_t n = (int64_t)jobs.size();
    std::atomic<int64_t> next{0};
    std::atomic<bool> stop{false};
    std::atomic<int64_t> bad{-1};
    std::atomic<int64_t> count{0};
    std::atomic<int64_t> sum{0};
    std::atomic<int64_t> odd{0};

    // Флаг проверяется до взятия номера, а взятый номер доделывается
    // всегда: так номер наименьшей плохой работы, взятый раньше любой
    // большей плохой, не пропадёт.
    auto work = [&] {
        for (;;) {
            if (stop.load()) return;
            const int64_t j = next.fetch_add(1);
            if (j >= n) return;
            const int64_t v = jobs[(size_t)j];
            if (v < 0) {
                store_min(bad, j);
                stop.store(true);
                continue;
            }
            const int64_t r = cost(v);
            count.fetch_add(1);
            sum.fetch_add(r);
            if (r % 2 != 0) odd.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve((size_t)workers);
    for (int32_t w = 0; w < workers; w++) threads.emplace_back(work);
    for (std::thread& t : threads) t.join();

    Totals t;
    if (stop.load()) {
        t.ok = false;
        t.bad = bad.load();
        t.count = 0;
        t.sum = 0;
        t.odd = 0;
    } else {
        t.ok = true;
        t.bad = -1;
        t.count = count.load();
        t.sum = sum.load();
        t.odd = odd.load();
    }
    return t;
}
