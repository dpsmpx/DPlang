#include "world.hpp"

UnitId spawn(World& w, int32_t x, int32_t y, int32_t hp, int32_t armor) {
    w.units.push_back({x, y, hp, armor});
    return {(int32_t)w.units.size() - 1};
}

std::optional<int32_t> unit_hp(const World& w, UnitId id) {
    if (id.index < 0 || id.index >= (int32_t)w.units.size()) return std::nullopt;
    return w.units[id.index].hp;
}
