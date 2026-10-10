// t11: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

struct Particle { int32_t x, y, vx, vy; };

static constexpr int64_t kGravity = -6554;

void step(std::vector<Particle>& ps) {
    for (Particle& p : ps) {
        int64_t vy = static_cast<int64_t>(p.vy) + kGravity;
        int64_t vx = p.vx;
        int64_t x = static_cast<int64_t>(p.x) + vx;
        int64_t y = static_cast<int64_t>(p.y) + vy;
        if (y < 0) {
            y = 0;
            vy = -(vy * 3) / 4;
            vx = vx * 7 / 8;
        }
        p.x = static_cast<int32_t>(x);
        p.y = static_cast<int32_t>(y);
        p.vx = static_cast<int32_t>(vx);
        p.vy = static_cast<int32_t>(vy);
    }
}

uint32_t checksum(const std::vector<Particle>& ps) {
    uint32_t h = 17;
    for (const Particle& p : ps) {
        for (int32_t f : {p.x, p.y, p.vx, p.vy}) {
            h = h * 31u + static_cast<uint32_t>(f);
        }
    }
    return h;
}
