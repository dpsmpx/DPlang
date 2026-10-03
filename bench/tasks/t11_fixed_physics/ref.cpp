// Эталон t11 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

struct Particle { int32_t x, y, vx, vy; };

constexpr int32_t G = -6554;

void step(std::vector<Particle>& ps) {
    for (Particle& p : ps) {
        p.vy = p.vy + G;
        p.x = p.x + p.vx;
        p.y = p.y + p.vy;
        if (p.y < 0) {
            p.y = 0;
            p.vy = (int32_t)(-((int64_t)p.vy * 3) / 4);
            p.vx = (int32_t)((int64_t)p.vx * 7 / 8);
        }
    }
}

uint32_t checksum(const std::vector<Particle>& ps) {
    uint32_t h = 17;
    for (const Particle& p : ps)
        for (int32_t f : {p.x, p.y, p.vx, p.vy}) h = h * 31u + (uint32_t)f;
    return h;
}

// --- driver ---
int main() {
    std::vector<Particle> ps = {
        {0, 10 * 65536, 65536, 0},
        {5 * 65536, 0, -32768, 3 * 65536},
        {-65536, 2 * 65536, 0, -65536},
    };
    for (int t = 1; t <= 120; t++) {
        step(ps);
        if (t % 30 == 0) {
            std::printf("tick %d\n", t);
            for (const Particle& p : ps) std::printf("  %d %d %d %d\n", p.x, p.y, p.vx, p.vy);
        }
    }
    std::printf("checksum %08x\n", checksum(ps));
    return 0;
}
