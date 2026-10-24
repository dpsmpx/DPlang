// Эталон М3 на C++ (-O2): тот же кадр и итог, что у bench/frame/frame.dpm.
// Варианты: fresh — контейнеры заново в каждом кадре; reuse — clear();
// arena — std::pmr::monotonic_buffer_resource, сброс в каждом кадре.
// Выделения считает замена operator new; при DPL_ALLOC_STATS — в stderr.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <new>
#include <unordered_map>
#include <vector>

static int64_t g_count, g_bytes;

void* operator new(std::size_t n) {
    g_count++;
    g_bytes += (int64_t)n;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

struct Pair {
    int64_t cell;
    int32_t obj;
};

static uint64_t mix(uint64_t x) {
    uint64_t h = x * 11400714819323198485ull;
    h = (h ^ (h >> 31)) * 13787848793156543929ull;
    return h ^ (h >> 29);
}

template <class V, class C, class P>
static int64_t frame(int64_t f, const std::vector<int64_t>& xs, const std::vector<int64_t>& ys, V& visible, C& cells,
                     P& pairs) {
    int64_t left = (f * 37) % 600000;
    for (size_t i = 0; i < xs.size(); i++) {
        int64_t x = (xs[i] + f * 101) % 1000000, y = (ys[i] + f * 53) % 1000000;
        if (x >= left && x < left + 400000 && y < 500000) {
            visible.push_back(int32_t(i));
            int64_t cell = (x / 10000) * 1000 + y / 10000;
            cells[cell] += 1;
            pairs.push_back(Pair{cell, int32_t(i)});
        }
    }
    uint64_t check = uint64_t(visible.size()) * 1000003 + uint64_t(cells.size());
    for (const Pair& p : pairs) check += uint64_t(p.cell * cells[p.cell]) + uint64_t(p.obj);
    return int64_t(check);
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "fresh";
    int64_t n = argc > 2 ? atoll(argv[2]) : 10000, frames = argc > 3 ? atoll(argv[3]) : 1000;
    std::vector<int64_t> xs, ys;
    uint64_t seed = 7;
    for (int64_t i = 0; i < n; i++) {
        seed = mix(seed + uint64_t(i));
        xs.push_back(int64_t(seed % 1000000));
        ys.push_back(int64_t((seed >> 24) % 1000000));
    }
    uint64_t total = 0;
    if (!strcmp(mode, "reuse")) {
        std::vector<int32_t> visible;
        std::unordered_map<int64_t, int32_t> cells;
        std::vector<Pair> pairs;
        for (int64_t f = 0; f < frames; f++) {
            visible.clear();
            cells.clear();
            pairs.clear();
            total += uint64_t(frame(f, xs, ys, visible, cells, pairs));
        }
    } else if (!strcmp(mode, "arena")) {
        std::vector<std::byte> buf(size_t(1) << 22);
        std::pmr::monotonic_buffer_resource arena(buf.data(), buf.size());
        for (int64_t f = 0; f < frames; f++) {
            {
                std::pmr::vector<int32_t> visible(&arena);
                std::pmr::unordered_map<int64_t, int32_t> cells(&arena);
                std::pmr::vector<Pair> pairs(&arena);
                total += uint64_t(frame(f, xs, ys, visible, cells, pairs));
            }
            arena.release();
        }
    } else {
        for (int64_t f = 0; f < frames; f++) {
            std::vector<int32_t> visible;
            std::unordered_map<int64_t, int32_t> cells;
            std::vector<Pair> pairs;
            total += uint64_t(frame(f, xs, ys, visible, cells, pairs));
        }
    }
    printf("%lld\n", (long long)int64_t(total));
    const char* on = getenv("DPL_ALLOC_STATS");
    if (on && *on) fprintf(stderr, "cpp: %lld allocations, %lld bytes\n", (long long)g_count, (long long)g_bytes);
}
