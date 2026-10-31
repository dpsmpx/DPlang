// t22: заготовка — напишите тело функции по task.md.
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

namespace {

// Расчёт 1: рабочие частями подряд, у каждого свой буфер отметок и свои
// счётчики; счётчики складываются после.
std::vector<int64_t> count_distinct(const std::vector<std::string>& words, int32_t workers) {
    const int64_t n = (int64_t)words.size();
    std::vector<std::array<int64_t, 27>> parts((size_t)workers);
    std::vector<std::thread> threads;
    threads.reserve((size_t)workers);
    for (int32_t w = 0; w < workers; w++) {
        const int64_t lo = n * w / workers;
        const int64_t hi = n * (w + 1) / workers;
        std::array<int64_t, 27>& cnt = parts[(size_t)w];
        cnt.fill(0);
        threads.emplace_back([&words, &cnt, lo, hi] {
            std::array<bool, 26> seen;  // буфер рабочего
            for (int64_t i = lo; i < hi; i++) {
                seen.fill(false);
                int32_t k = 0;
                for (unsigned char ch : words[(size_t)i]) {
                    if (ch >= 'a' && ch <= 'z' && !seen[ch - 'a']) {
                        seen[ch - 'a'] = true;
                        k++;
                    }
                }
                cnt[(size_t)k]++;
            }
        });
    }
    for (std::thread& t : threads) t.join();
    std::vector<int64_t> total(27, 0);
    for (const auto& cnt : parts) {
        for (size_t k = 0; k < 27; k++) total[k] += cnt[k];
    }
    return total;
}

// Расчёт 2: первое из самых длинных слов.
std::string find_longest(const std::vector<std::string>& words) {
    size_t best = 0;
    bool found = false;
    for (size_t i = 0; i < words.size(); i++) {
        if (!found || words[i].size() > words[best].size()) {
            best = i;
            found = true;
        }
    }
    return found ? words[best] : std::string();
}

// Расчёт 3: соседние пары-анаграммы по счётчикам байтов.
int64_t count_anagrams(const std::vector<std::string>& words) {
    int64_t pairs = 0;
    std::array<int64_t, 256> a;
    std::array<int64_t, 256> b;
    for (size_t i = 0; i + 1 < words.size(); i++) {
        const std::string& x = words[i];
        const std::string& y = words[i + 1];
        if (x.size() != y.size()) continue;
        a.fill(0);
        b.fill(0);
        for (unsigned char ch : x) a[ch]++;
        for (unsigned char ch : y) b[ch]++;
        if (a == b) pairs++;
    }
    return pairs;
}

}  // namespace

Stats analyze(const std::vector<std::string>& words, int32_t workers) {
    Stats s;
    std::thread t1([&] { s.distinct = count_distinct(words, workers); });
    std::thread t2([&] { s.longest = find_longest(words); });
    std::thread t3([&] { s.anagrams = count_anagrams(words); });
    t1.join();
    t2.join();
    t3.join();
    return s;
}
