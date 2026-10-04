// Модуль combat (до рефакторинга): урон.
#pragma once
#include "world.hpp"

// Урон после брони: max(1, dmg - armor); здоровье не опускается ниже 0,
// при 0 юнит погибает. Код: 0 — попадание, -1 — нет юнита, -2 — уже мёртв.
int hit(World& w, int id, int dmg);
// Бьёт всех живых в квадрате |x - cx| <= r, |y - cy| <= r по порядку
// номеров; возвращает число задетых.
int blast(World& w, int cx, int cy, int r, int dmg);
