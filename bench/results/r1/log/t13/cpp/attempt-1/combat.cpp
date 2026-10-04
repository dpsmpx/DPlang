#include "combat.hpp"

#include <cstdlib>

static int32_t after_armor(int32_t dmg, int32_t armor) {
    int32_t d = dmg - armor;
    return d < 1 ? 1 : d;
}

DamageResult apply_damage(World& w, UnitId id, int32_t dmg) {
    if (id.index < 0 || id.index >= (int32_t)w.units.size()) return {CombatError::NotFound, 0};
    Unit& u = w.units[id.index];
    if (u.hp == 0) return {CombatError::Dead, 0};
    u.hp -= after_armor(dmg, u.armor);
    if (u.hp < 0) u.hp = 0;
    return {CombatError::None, u.hp};
}

std::vector<UnitId> blast(World& w, int32_t cx, int32_t cy, int32_t r, int32_t dmg) {
    std::vector<UnitId> hits;
    for (int32_t i = 0; i < (int32_t)w.units.size(); i++) {
        const Unit& u = w.units[i];
        if (u.hp != 0 && std::abs(u.x - cx) <= r && std::abs(u.y - cy) <= r) {
            apply_damage(w, UnitId{i}, dmg);
            hits.push_back(UnitId{i});
        }
    }
    return hits;
}
