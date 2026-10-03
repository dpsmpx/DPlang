// Драйвер t11 для переведённых вариантов DPlang (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

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
