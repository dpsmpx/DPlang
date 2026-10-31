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

std::vector<std::string> run(const std::vector<Cmd>& script, int32_t limit) {
    // ...
}
