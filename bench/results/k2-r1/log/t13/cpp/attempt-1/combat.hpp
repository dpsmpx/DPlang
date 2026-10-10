// Модуль combat (после рефакторинга): урон.
#pragma once
#include <cstdint>
#include <vector>

#include "world.hpp"

enum class CombatError { None, NotFound, Dead };
struct DamageResult { CombatError error; int32_t hp; };  // hp — после удара

// Урон после брони: max(1, dmg - armor); здоровье не опускается ниже 0.
DamageResult apply_damage(World& w, UnitId id, int32_t dmg);
// Бьёт всех живых в квадрате |x - cx| <= r, |y - cy| <= r по порядку
// номеров; возвращает задетых в том же порядке.
std::vector<UnitId> blast(World& w, int32_t cx, int32_t cy, int32_t r, int32_t dmg);
