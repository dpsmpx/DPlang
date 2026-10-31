// t24: заготовка — напишите тело функции по task.md.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

struct Move {
    int32_t from, to;
    int64_t amount;
};

struct Report {
    std::vector<int64_t> table;
    int64_t reads = 0, bad = 0;
};

Report run_bank(int32_t accounts, const std::vector<Move>& moves, int32_t readers, int32_t reads) {
    // ...
}
