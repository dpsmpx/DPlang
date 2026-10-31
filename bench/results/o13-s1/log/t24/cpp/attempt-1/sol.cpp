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
    std::vector<int64_t> table((size_t)accounts, 1000);
    std::shared_mutex mu;  // перевод — под исключительной, чтение — под общей
    const int64_t expected = (int64_t)accounts * 1000;
    std::vector<int64_t> bad_by_reader((size_t)readers, 0);
    std::vector<int64_t> done_by_reader((size_t)readers, 0);

    std::thread writer([&] {
        for (const Move& m : moves) {
            if (m.from == m.to) continue;
            std::unique_lock<std::shared_mutex> lk(mu);
            int64_t& src = table[(size_t)m.from];
            int64_t& dst = table[(size_t)m.to];
            const int64_t x = m.amount < src ? m.amount : src;
            src -= x;
            dst += x;
        }
    });

    std::vector<std::thread> rs;
    rs.reserve((size_t)readers);
    for (int32_t r = 0; r < readers; r++) {
        rs.emplace_back([&, r] {
            int64_t bad = 0;
            int64_t done = 0;
            for (int32_t i = 0; i < reads; i++) {
                int64_t total = 0;
                {
                    std::shared_lock<std::shared_mutex> lk(mu);
                    for (int64_t v : table) total += v;
                }
                if (total != expected) bad++;
                done++;
            }
            bad_by_reader[(size_t)r] = bad;
            done_by_reader[(size_t)r] = done;
        });
    }

    writer.join();
    for (std::thread& t : rs) t.join();

    Report rep;
    rep.table = table;
    for (int32_t r = 0; r < readers; r++) {
        rep.reads += done_by_reader[(size_t)r];
        rep.bad += bad_by_reader[(size_t)r];
    }
    return rep;
}
