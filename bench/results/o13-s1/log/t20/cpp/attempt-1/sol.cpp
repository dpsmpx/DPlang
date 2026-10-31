// t20: заготовка — напишите тело run по task.md.
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int64_t build(int64_t chunk, uint64_t seed) {
    uint64_t h = (uint64_t)chunk ^ seed;
    for (int32_t i = 0; i < 20000; i++) h = mix(h);
    return (int64_t)(h % 100000);
}

struct Cmd {
    int32_t kind;
    int64_t chunk;
    uint64_t seed;
};

namespace {

struct Job {
    int64_t id;
    int64_t chunk;
    uint64_t seed;
};

struct Result {
    int64_t id;
    int64_t chunk;
    int64_t value;
};

// Общее состояние менеджера и рабочих. Рабочие держат его через
// shared_ptr, поэтому оно живёт, пока жив последний из них, даже после
// ухода менеджера.
struct Shared {
    std::mutex m;
    std::condition_variable work_cv;  // появилась работа или закрытие
    std::condition_variable done_cv;  // закончена сборка
    std::deque<Job> jobs;             // заказы, ещё не взятые рабочими
    std::deque<Result> ready;         // готовые, ещё не забранные
    int64_t done = 0;                 // законченных сборок
    bool closed = false;              // менеджер ушёл или закончил
};

void worker(std::shared_ptr<Shared> st) {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(st->m);
            st->work_cv.wait(lk, [&] { return st->closed || !st->jobs.empty(); });
            if (st->jobs.empty()) return;  // закрыто и работы нет
            j = st->jobs.front();
            st->jobs.pop_front();
        }
        int64_t v = build(j.chunk, j.seed);
        {
            std::lock_guard<std::mutex> lk(st->m);
            if (!st->closed) st->ready.push_back(Result{j.id, j.chunk, v});
            st->done++;
        }
        st->done_cv.notify_all();
    }
}

constexpr int32_t kWorkers = 4;

}  // namespace

std::vector<std::string> run(const std::vector<Cmd>& script, int32_t limit) {
    std::vector<std::string> out;
    std::shared_ptr<Shared> st = std::make_shared<Shared>();
    bool started = false;
    int64_t ordered = 0;
    int64_t syncs = 0;
    std::map<int64_t, int64_t> latest;   // чанк -> номер последнего заказа
    std::map<int64_t, int64_t> applied;  // чанк -> применённое значение

    auto apply = [&](const std::vector<Result>& got) {
        for (const Result& r : got) {
            auto it = latest.find(r.chunk);
            if (it != latest.end() && it->second == r.id) applied[r.chunk] = r.value;
        }
    };

    auto close = [&] {
        {
            std::lock_guard<std::mutex> lk(st->m);
            st->closed = true;
            st->jobs.clear();
            st->ready.clear();
        }
        st->work_cv.notify_all();
    };

    for (const Cmd& c : script) {
        if (c.kind == 0) {
            if (!started) {
                started = true;
                for (int32_t i = 0; i < kWorkers; i++) std::thread(worker, st).detach();
            }
            ordered++;
            latest[c.chunk] = ordered;
            {
                std::lock_guard<std::mutex> lk(st->m);
                st->jobs.push_back(Job{ordered, c.chunk, c.seed});
            }
            st->work_cv.notify_one();
        } else if (c.kind == 1) {
            std::vector<Result> got;
            {
                std::lock_guard<std::mutex> lk(st->m);
                while (!st->ready.empty() && (int64_t)got.size() < (int64_t)limit) {
                    got.push_back(st->ready.front());
                    st->ready.pop_front();
                }
            }
            apply(got);
        } else if (c.kind == 2) {
            std::vector<Result> got;
            {
                std::unique_lock<std::mutex> lk(st->m);
                st->done_cv.wait(lk, [&] { return st->done == ordered; });
                got.assign(st->ready.begin(), st->ready.end());
                st->ready.clear();
            }
            apply(got);
            syncs++;
            std::string line = "sync " + std::to_string(syncs) + ":";
            for (const auto& [chunk, value] : applied) {
                line += " " + std::to_string(chunk) + "=" + std::to_string(value);
            }
            out.push_back(line);
        } else if (c.kind == 3) {
            break;
        }
    }
    close();
    return out;
}
