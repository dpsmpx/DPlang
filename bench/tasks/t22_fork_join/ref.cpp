// Эталон t22 на C++20. Над чертой — решение, под чертой — драйвер.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

struct Stats {
    std::vector<int64_t> distinct;
    std::string longest;
    int64_t anagrams = 0;
};

static void count_part(const std::vector<std::string>& words, int32_t w, int32_t workers, std::vector<int64_t>& out) {
    int64_t n = (int64_t)words.size();
    std::array<bool, 26> seen{};  // буфер рабочего: очищается перед словом
    for (int64_t i = n * w / workers; i < n * (w + 1) / workers; i++) {
        seen.fill(false);
        int32_t k = 0;
        for (char c : words[i]) {
            if (c >= 'a' && c <= 'z' && !seen[c - 'a']) {
                seen[c - 'a'] = true;
                k++;
            }
        }
        out[k]++;
    }
}

static bool anagram(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    std::array<int32_t, 256> c{};
    for (unsigned char x : a) c[x]++;
    for (unsigned char x : b) c[x]--;
    for (int32_t v : c)
        if (v != 0) return false;
    return true;
}

Stats analyze(const std::vector<std::string>& words, int32_t workers) {
    Stats s;
    std::vector<std::vector<int64_t>> parts(workers, std::vector<int64_t>(27, 0));
    std::vector<std::thread> pool;
    for (int32_t w = 0; w < workers; w++) pool.emplace_back([&, w] { count_part(words, w, workers, parts[w]); });
    pool.emplace_back([&] {
        for (const std::string& x : words)
            if (x.size() > s.longest.size()) s.longest = x;
    });
    pool.emplace_back([&] {
        for (size_t i = 0; i + 1 < words.size(); i++)
            if (anagram(words[i], words[i + 1])) s.anagrams++;
    });
    for (std::thread& th : pool) th.join();
    s.distinct.assign(27, 0);
    for (const std::vector<int64_t>& p : parts)
        for (int32_t k = 0; k < 27; k++) s.distinct[k] += p[k];
    return s;
}

// --- driver ---
static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void print(const char* name, const Stats& s) {
    std::printf("%s", name);
    for (int64_t c : s.distinct) std::printf(" %lld", (long long)c);
    std::printf(" | %s | %lld\n", s.longest.c_str(), (long long)s.anagrams);
}

int main() {
    std::vector<std::string> words;
    for (uint64_t i = 0; i < 150000; i++) {
        uint64_t h = mix(i);
        int32_t len = (int32_t)(h % 7);
        std::string w;
        for (int32_t k = 0; k < len; k++) {
            h = mix(h);
            w += (char)('a' + h % (k % 2 == 0 ? 4 : 26));
        }
        if (i % 9 == 0 && !words.empty()) w = std::string(words.back().rbegin(), words.back().rend());
        if (i == 77777) w = "the-longest-word-of-all";
        words.push_back(w);
    }
    for (int32_t t : {1, 4, 7}) {
        char name[16];
        std::snprintf(name, sizeof name, "w=%d", t);
        print(name, analyze(words, t));
    }
    print("empty", analyze({}, 3));
    print("small", analyze({"ab", "ba", "x", "x", "Zz9"}, 2));
    return 0;
}
