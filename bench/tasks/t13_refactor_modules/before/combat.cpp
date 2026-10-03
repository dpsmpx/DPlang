#include "combat.hpp"

#include <cstdlib>

static int after_armor(int dmg, int armor) {
    int d = dmg - armor;
    return d < 1 ? 1 : d;
}

int hit(World& w, int id, int dmg) {
    if (id < 0 || id >= (int)w.units.size()) return -1;
    Unit& u = w.units[id];
    if (!u.alive) return -2;
    u.hp -= after_armor(dmg, u.armor);
    if (u.hp <= 0) {
        u.hp = 0;
        u.alive = false;
    }
    return 0;
}

int blast(World& w, int cx, int cy, int r, int dmg) {
    int n = 0;
    for (int i = 0; i < (int)w.units.size(); i++) {
        const Unit& u = w.units[i];
        if (u.alive && std::abs(u.x - cx) <= r && std::abs(u.y - cy) <= r) {
            hit(w, i, dmg);
            n++;
        }
    }
    return n;
}
