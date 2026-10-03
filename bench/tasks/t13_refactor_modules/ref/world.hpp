// Модуль world (после рефакторинга): юниты на плоскости.
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

struct UnitId { int32_t index; };
struct Unit { int32_t x, y, hp, armor; };  // hp == 0 — юнит мёртв
struct World { std::vector<Unit> units; };

// Добавляет живого юнита (hp >= 1).
UnitId spawn(World& w, int32_t x, int32_t y, int32_t hp, int32_t armor);
// Здоровье юнита; нет значения, если такого номера нет.
std::optional<int32_t> unit_hp(const World& w, UnitId id);
