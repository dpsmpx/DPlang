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

Stats analyze(const std::vector<std::string>& words, int32_t workers) {
    // ...
}
