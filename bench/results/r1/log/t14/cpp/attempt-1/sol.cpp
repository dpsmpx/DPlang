// t14: заготовка — напишите тела функций по task.md.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr size_t kBlock = 4096;

// Число рабочих потоков: не меньше 1 и не больше числа единиц работы.
size_t worker_count(int32_t threads, size_t units) {
    size_t t = threads < 1 ? 1 : static_cast<size_t>(threads);
    if (units == 0) return 1;
    return std::min(t, units);
}

}  // namespace

double parallel_sum(const std::vector<double>& v, int32_t threads) {
    const size_t n = v.size();
    if (n == 0) return 0.0;
    const size_t blocks = (n + kBlock - 1) / kBlock;
    // Каждый элемент block_sums пишет ровно один поток; читается после join.
    std::vector<double> block_sums(blocks, 0.0);
    const size_t workers = worker_count(threads, blocks);

    auto work = [&v, &block_sums, n, blocks, workers](size_t w) {
        // Потоку w — непрерывный диапазон блоков [first, last).
        const size_t first = blocks * w / workers;
        const size_t last = blocks * (w + 1) / workers;
        for (size_t b = first; b < last; ++b) {
            const size_t lo = b * kBlock;
            const size_t hi = std::min(n, lo + kBlock);
            double s = 0.0;
            for (size_t i = lo; i < hi; ++i) s += v[i];
            block_sums[b] = s;
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) pool.emplace_back(work, w);
    for (auto& t : pool) t.join();

    // Слияние — в фиксированном порядке блоков, слева направо.
    double total = 0.0;
    for (size_t b = 0; b < blocks; ++b) total += block_sums[b];
    return total;
}

std::vector<int64_t> parallel_histogram(const std::vector<uint32_t>& v, int32_t buckets, int32_t threads) {
    const size_t nb = buckets < 1 ? 1 : static_cast<size_t>(buckets);
    const uint32_t mod = static_cast<uint32_t>(nb);
    std::vector<int64_t> result(nb, 0);
    const size_t n = v.size();
    if (n == 0) return result;

    const size_t workers = worker_count(threads, n);
    // Локальные счётчики: у каждого потока свой вектор.
    std::vector<std::vector<int64_t>> local(workers, std::vector<int64_t>(nb, 0));

    auto work = [&v, &local, n, workers, mod](size_t w) {
        const size_t lo = n * w / workers;
        const size_t hi = n * (w + 1) / workers;
        std::vector<int64_t>& cnt = local[w];
        for (size_t i = lo; i < hi; ++i) ++cnt[v[i] % mod];
    };

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) pool.emplace_back(work, w);
    for (auto& t : pool) t.join();

    // Слияние после join, в порядке потоков.
    for (size_t w = 0; w < workers; ++w)
        for (size_t k = 0; k < nb; ++k) result[k] += local[w][k];
    return result;
}
