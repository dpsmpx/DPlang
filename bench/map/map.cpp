// Эталон замера Map (О12; bench/map/measure.py): те же фазы и вывод, что у
// map.dpm. -DUNORDERED — std::unordered_map вместо std::map.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#ifdef UNORDERED
template <class K, class V> using Table = std::unordered_map<K, V>;
#else
template <class K, class V> using Table = std::map<K, V>;
#endif

static uint64_t mix(uint64_t x) {
    uint64_t h = x * 11400714819323198485ull;
    h = (h ^ (h >> 31)) * 13787848793156543929ull;
    return h ^ (h >> 29);
}

static int64_t ikey(int64_t i) { return (int64_t)(mix((uint64_t)i) >> 20); }
static std::string skey(int64_t i) { return "key" + std::to_string(mix((uint64_t)i) % 1000000000); }
static int64_t len_of(int64_t) { return 0; }
static int64_t len_of(const std::string& s) { return (int64_t)s.size(); }

template <class K, class F>
static void run(const char* phase, int64_t n, F key) {
    Table<K, int64_t> m;
    for (int64_t i = 0; i < n; i++) m[key(i)] = i;
    uint64_t check = 0;
    if (!strcmp(phase, "lookup")) {
        for (int64_t r = 0; r < 5; r++) {
            for (int64_t i = 0; i < n; i++) {
                auto it = m.find(key(i + (r % 2) * n));
                check += it == m.end() ? 0 : (uint64_t)it->second;
            }
        }
    }
    if (!strcmp(phase, "iter")) {
        for (int r = 0; r < 20; r++) {
            for (const auto& [k, v] : m) {
                if constexpr (std::is_same_v<K, int64_t>) check += (uint64_t)v + (uint64_t)k;
                else check += (uint64_t)v + (uint64_t)len_of(k);
            }
        }
    }
    if (!strcmp(phase, "churn")) {
        for (int64_t i = 1; i < n; i += 2) {
            auto it = m.find(key(i));
            if (it != m.end()) {
                check += (uint64_t)it->second;
                m.erase(it);
            }
        }
        for (int64_t i = n; i < n + n / 2; i++) m[key(i)] = i;
    }
    printf("%lld %lld\n", (long long)m.size(), (long long)(int64_t)check);
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    int64_t n = atoll(argv[3]);
    if (!strcmp(argv[1], "int")) run<int64_t>(argv[2], n, ikey);
    else run<std::string>(argv[2], n, skey);
}
