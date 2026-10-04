// Модуль world (до рефакторинга): юниты на плоскости.
#pragma once
#include <vector>

struct Unit { int x, y, hp, armor; bool alive; };
struct World { std::vector<Unit> units; };

// Добавляет живого юнита (hp >= 1); возвращает его номер.
int spawn(World& w, int x, int y, int hp, int armor);
// Здоровье юнита; -1, если такого номера нет.
int unit_hp(const World& w, int id);
