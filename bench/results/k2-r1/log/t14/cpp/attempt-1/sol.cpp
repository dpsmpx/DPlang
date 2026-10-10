// t14: заготовка — напишите тела функций по task.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

static constexpr size_t kBlock = 4096;

double parallel_sum(const std::vector<double>& v, int32_t threads) {
    if (v.empty()) return 0.0;
    size_t n = v.size();
    size_t nblocks = (n + kBlock - 1) / kBlock;
    size_t nt = threads < 1 ? 1 : static_cast<size_t>(threads);
    // Каждый блок пишет только в свою ячейку; ячейки разных потоков не пересекаются.
    std::vector<double> sums(nblocks, 0.0);
    auto work = [&](size_t t) {
        for (size_t b = t; b < nblocks; b += nt) {
            size_t lo = b * kBlock;
            size_t hi = std::min(n, lo + kBlock);
            double s = 0.0;
            for (size_t i = lo; i < hi; ++i) s += v[i];
            sums[b] = s;
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (size_t t = 0; t < nt; ++t) pool.emplace_back(work, t);
    for (auto& th : pool) th.join();
    double total = 0.0;
    for (size_t b = 0; b < nblocks; ++b) total += sums[b];
    return total;
}

std::vector<int64_t> parallel_histogram(const std::vector<uint32_t>& v, int32_t buckets, int32_t threads) {
    size_t nb = buckets < 1 ? 1 : static_cast<size_t>(buckets);
    uint32_t m = static_cast<uint32_t>(nb);
    size_t nt = threads < 1 ? 1 : static_cast<size_t>(threads);
    size_t n = v.size();
    // Локальные счётчики у каждого потока, слияние после join.
    std::vector<std::vector<int64_t>> local(nt, std::vector<int64_t>(nb, 0));
    size_t chunk = (n + nt - 1) / nt;
    auto work = [&](size_t t) {
        size_t lo = std::min(n, t * chunk);
        size_t hi = std::min(n, lo + chunk);
        std::vector<int64_t>& c = local[t];
        for (size_t i = lo; i < hi; ++i) c[v[i] % m] += 1;
    };
    std::vector<std::thread> pool;
    pool.reserve(nt);
    for (size_t t = 0; t < nt; ++t) pool.emplace_back(work, t);
    for (auto& th : pool) th.join();
    std::vector<int64_t> out(nb, 0);
    for (size_t t = 0; t < nt; ++t)
        for (size_t k = 0; k < nb; ++k) out[k] += local[t][k];
    return out;
}
