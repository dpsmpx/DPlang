#include "world.hpp"

int spawn(World& w, int x, int y, int hp, int armor) {
    w.units.push_back({x, y, hp, armor, true});
    return (int)w.units.size() - 1;
}

int unit_hp(const World& w, int id) {
    if (id < 0 || id >= (int)w.units.size()) return -1;
    return w.units[id].hp;
}
