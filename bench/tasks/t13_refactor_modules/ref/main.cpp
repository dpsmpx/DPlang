// Драйвер после рефакторинга (новый интерфейс). Вывод — тот же, что до.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "combat.hpp"
#include "report.hpp"
#include "world.hpp"

static int32_t hp_or_minus1(const World& w, UnitId id) {
    std::optional<int32_t> hp = unit_hp(w, id);
    return hp ? *hp : -1;
}

static void do_hit(World& w, UnitId id, int32_t dmg) {
    DamageResult r = apply_damage(w, id, dmg);
    if (r.error == CombatError::NotFound) std::printf("hit %d %d: not_found\n", id.index, dmg);
    else if (r.error == CombatError::Dead) std::printf("hit %d %d: dead\n", id.index, dmg);
    else std::printf("hit %d %d: hp %d\n", id.index, dmg, r.hp);
}

static void do_blast(World& w, int32_t cx, int32_t cy, int32_t r, int32_t dmg) {
    std::vector<UnitId> hits = blast(w, cx, cy, r, dmg);
    std::printf("blast (%d,%d) r%d %d: %d hit:", cx, cy, r, dmg, (int)hits.size());
    for (UnitId id : hits) std::printf(" %d", id.index);
    std::printf(" | %s | alive %d\n", describe(w).c_str(), alive_count(w));
}

int main() {
    World w;
    UnitId a = spawn(w, 0, 0, 30, 2);
    UnitId b = spawn(w, 3, 1, 12, 0);
    UnitId c = spawn(w, 10, 10, 50, 10);
    UnitId d = spawn(w, 1, -1, 5, 1);
    std::printf("%s\n", describe(w).c_str());
    do_hit(w, a, 10);
    do_hit(w, c, 4);
    do_hit(w, UnitId{7}, 1);
    do_blast(w, 1, 0, 2, 4);
    do_hit(w, d, 3);
    do_hit(w, d, 3);
    do_blast(w, 2, 0, 2, 14);
    do_blast(w, 10, 10, 0, 100);
    do_hit(w, b, 1);
    std::printf("hp %d %d %d\n", hp_or_minus1(w, a), hp_or_minus1(w, UnitId{9}), hp_or_minus1(w, UnitId{-1}));
    return 0;
}
