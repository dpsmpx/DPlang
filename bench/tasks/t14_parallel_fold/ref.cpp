// Эталон t14 на C++20. Над чертой — решение, под чертой — драйвер.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

constexpr int32_t BLOCK = 4096;

double parallel_sum(const std::vector<double>& v, int32_t threads) {
    int32_t n = (int32_t)v.size();
    int32_t blocks = (n + BLOCK - 1) / BLOCK;
    std::vector<double> partial(blocks, 0.0);
    std::vector<std::thread> pool;
    for (int32_t t = 0; t < threads; t++) {
        pool.emplace_back([&v, &partial, n, blocks, threads, t] {
            for (int32_t b = t; b < blocks; b += threads) {
                int32_t lo = b * BLOCK, hi = std::min(n, lo + BLOCK);
                double s = 0.0;
                for (int32_t i = lo; i < hi; i++) s += v[i];
                partial[b] = s;
            }
        });
    }
    for (std::thread& th : pool) th.join();
    double s = 0.0;
    for (double p : partial) s += p;
    return s;
}

std::vector<int64_t> parallel_histogram(const std::vector<uint32_t>& v, int32_t buckets, int32_t threads) {
    int32_t n = (int32_t)v.size();
    std::vector<std::vector<int64_t>> local(threads, std::vector<int64_t>(buckets, 0));
    std::vector<std::thread> pool;
    for (int32_t t = 0; t < threads; t++) {
        pool.emplace_back([&v, &local, n, buckets, threads, t] {
            int32_t lo = (int32_t)((int64_t)n * t / threads);
            int32_t hi = (int32_t)((int64_t)n * (t + 1) / threads);
            for (int32_t i = lo; i < hi; i++) local[t][v[i] % (uint32_t)buckets]++;
        });
    }
    for (std::thread& th : pool) th.join();
    std::vector<int64_t> out(buckets, 0);
    for (const std::vector<int64_t>& h : local)
        for (int32_t b = 0; b < buckets; b++) out[b] += h[b];
    return out;
}

// --- driver ---
static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main() {
    const int32_t n = 100003;
    std::vector<double> v(n);
    std::vector<uint32_t> u(n);
    for (int32_t i = 0; i < n; i++) {
        uint64_t h = mix((uint64_t)i);
        double unit = (double)(h >> 11) / 9007199254740992.0;  // [0, 1)
        v[i] = (unit * 2.0 - 1.0) * (double)(1ull << (i % 40));
        u[i] = (uint32_t)(h >> 32);
    }
    for (int32_t t : {1, 2, 3, 8}) std::printf("sum t=%d %.17g\n", t, parallel_sum(v, t));
    std::printf("sum small %.17g\n", parallel_sum(std::vector<double>{0.5, 0.25, -1.0}, 4));
    std::printf("sum empty %.17g\n", parallel_sum(std::vector<double>{}, 2));
    for (int32_t t : {1, 5}) {
        std::vector<int64_t> h = parallel_histogram(u, 10, t);
        int64_t total = 0;
        std::printf("hist t=%d", t);
        for (int64_t c : h) { std::printf(" %lld", (long long)c); total += c; }
        std::printf(" total %lld\n", (long long)total);
    }
    std::vector<int64_t> h = parallel_histogram(std::vector<uint32_t>{7, 17, 3}, 5, 8);
    std::printf("hist small %lld %lld %lld %lld %lld\n", (long long)h[0], (long long)h[1],
                (long long)h[2], (long long)h[3], (long long)h[4]);
    return 0;
}
