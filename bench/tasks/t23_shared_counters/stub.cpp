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

Totals run_jobs(const std::vector<int64_t>& jobs, int32_t workers) {
    // ...
}
