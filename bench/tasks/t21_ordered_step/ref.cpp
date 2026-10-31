// Эталон t21 на C++20. Над чертой — решение, под чертой — драйвер.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

constexpr double K = 0.25;
constexpr double DT = 0.1;

std::vector<double> simulate(const std::vector<double>& x0, int32_t steps, int32_t threads) {
    int32_t n = (int32_t)x0.size();
    std::vector<double> x = x0, v(n, 0.0), nx(n), nv(n);
    std::vector<double> out;
    for (int32_t s = 0; s < steps; s++) {
        std::vector<std::thread> pool;
        for (int32_t t = 0; t < threads; t++) {
            pool.emplace_back([&, t] {
                int32_t lo = (int32_t)((int64_t)n * t / threads), hi = (int32_t)((int64_t)n * (t + 1) / threads);
                for (int32_t i = lo; i < hi; i++) {
                    double l = i > 0 ? x[i - 1] : 0.0, r = i + 1 < n ? x[i + 1] : 0.0;
                    double f = K * (l - 2.0 * x[i] + r);
                    nv[i] = v[i] + f * DT;
                    nx[i] = x[i] + nv[i] * DT;
                }
            });
        }
        for (std::thread& th : pool) th.join();
        std::swap(x, nx);
        std::swap(v, nv);
        double e = 0.0;
        for (int32_t i = 0; i < n; i++) {
            double d = x[i] - (i > 0 ? x[i - 1] : 0.0);
            e = e + 0.5 * v[i] * v[i];
            e = e + 0.5 * K * d * d;
        }
        if (n > 0) e = e + 0.5 * K * x[n - 1] * x[n - 1];
        out.push_back(e);
    }
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
    const int32_t n = 200003;
    std::vector<double> x(n);
    for (int32_t i = 0; i < n; i++) x[i] = (double)(mix((uint64_t)i) >> 11) / 9007199254740992.0 * 2.0 - 1.0;
    std::vector<double> e1 = simulate(x, 12, 1);
    for (size_t s = 0; s < e1.size(); s++) std::printf("step %zu %.17g\n", s, e1[s]);
    for (int32_t t : {3, 8}) std::printf("t=%d %s\n", t, simulate(x, 12, t) == e1 ? "same" : "differs");
    std::vector<double> one = simulate(std::vector<double>{1.0}, 3, 4);
    std::printf("one %.17g %.17g %.17g\n", one[0], one[1], one[2]);
    std::vector<double> three = simulate(std::vector<double>{0.5, -0.25, 1.0}, 2, 2);
    std::printf("three %.17g %.17g\n", three[0], three[1]);
    std::printf("empty %zu none %zu\n", simulate(std::vector<double>{}, 2, 3).size(), simulate(x, 0, 2).size());
    return 0;
}
