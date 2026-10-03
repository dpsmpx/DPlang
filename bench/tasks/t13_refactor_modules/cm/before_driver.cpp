// Драйвер t13 до рефакторинга (старый интерфейс) для переведённых вариантов DPlang.
#include "cm_out.hpp"

static void do_hit(World& w, int32_t id, int32_t dmg) {
    int32_t code = hit(w, id, dmg);
    if (code == -1) std::printf("hit %d %d: not_found\n", id, dmg);
    else if (code == -2) std::printf("hit %d %d: dead\n", id, dmg);
    else std::printf("hit %d %d: hp %d\n", id, dmg, unit_hp(w, id));
}

static void do_blast(World& w, int32_t cx, int32_t cy, int32_t r, int32_t dmg) {
    std::vector<int32_t> before;
    for (int32_t i = 0; i < (int32_t)w.units.size(); i++) before.push_back(unit_hp(w, i));
    int32_t n = blast(w, cx, cy, r, dmg);
    std::printf("blast (%d,%d) r%d %d: %d hit:", cx, cy, r, dmg, n);
    for (int32_t i = 0; i < (int32_t)w.units.size(); i++)
        if (unit_hp(w, i) != before[i]) std::printf(" %d", i);
    std::printf(" | %s | alive %d\n", describe(w).c_str(), alive_count(w));
}

int main() {
    World w;
    int32_t a = spawn(w, 0, 0, 30, 2);
    int32_t b = spawn(w, 3, 1, 12, 0);
    int32_t c = spawn(w, 10, 10, 50, 10);
    int32_t d = spawn(w, 1, -1, 5, 1);
    std::printf("%s\n", describe(w).c_str());
    do_hit(w, a, 10);
    do_hit(w, c, 4);
    do_hit(w, 7, 1);
    do_blast(w, 1, 0, 2, 4);
    do_hit(w, d, 3);
    do_hit(w, d, 3);
    do_blast(w, 2, 0, 2, 14);
    do_blast(w, 10, 10, 0, 100);
    do_hit(w, b, 1);
    std::printf("hp %d %d %d\n", unit_hp(w, a), unit_hp(w, 9), unit_hp(w, -1));
    return 0;
}
