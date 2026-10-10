# Задача t13 на C++

Задание опыта: написать код по постановке.

## Правила

- Работай только в папке `/home/user/C-/build/experiment/k2-r1/t13/cpp`: читай этот файл и `src/`, правь файлы в `src/`. Других файлов не открывай, по диску и в сети не ищи.
- Компилятор и другие программы сам не запускай. Проверка — только командой

      python3 /home/user/C-/bench/experiment.py check /home/user/C-/build/experiment/k2-r1/t13/cpp

  Она собирает его со скрытым драйвером, запускает и сравнивает вывод с ожидаемым; печатает «принято» или первую ошибку.
- Попыток проверки — не больше 5, каждая записывается. Проверяй, когда решение готово целиком, а не для разведки.
- Работа кончается на «принято» или когда попытки кончились. Ответ — одна строка: номер последней попытки и её итог.

## Код

В `src/combat.cpp`, `src/combat.hpp`, `src/report.cpp`, `src/report.hpp`, `src/world.cpp`, `src/world.hpp` — модули до рефакторинга. Измени их так, чтобы интерфейс стал как в разделе «Целевой интерфейс»; поведение не меняется.

Сборка — `g++ -std=c++20 -O1 -pthread -Wall -Wextra -Werror -fno-exceptions -fno-rtti`, с ASan и UBSan. Драйвер `main.cpp` подключает заголовки модулей и собирается вместе со всеми `src/*.cpp`.

## t13. Рефакторинг через несколько модулей

Дана программа из трёх модулей — `world`, `combat`, `report` (начальный
код задачи). Нужно изменить интерфейс модулей так, чтобы собирался и
проходил **новый драйвер**; целевой интерфейс модулей приложен к
заданию. Поведение не меняется: вывод нового драйвера совпадает с
выводом старого. Мера задачи — точность согласованной правки в
нескольких модулях.

### Что изменить

1. Номер юнита — отдельный тип `UnitId { index: i32 }` вместо голого
   `i32` во всём интерфейсе (`spawn`, `unit_hp`, урон).
2. `unit_hp(w: &World, id: UnitId) -> optional i32` — нет значения вместо
   `-1`.
3. `hit` переименовать в `apply_damage(w: &mut World, id: UnitId, dmg: i32)
   -> ошибка NotFound | Dead | i32` — здоровье после удара вместо кода
   `0` / `-1` / `-2`.
4. `blast(...)` возвращает список задетых `UnitId` по порядку номеров
   вместо их числа.
5. Поле `alive` убрать: юнит мёртв тогда и только тогда, когда `hp == 0`.

Прочее (`describe`, `alive_count`, правила урона) — без изменений.

### Проверки

Драйвер — `expected.txt`; старый драйвер со старыми модулями даёт тот же
вывод. Новый драйвер со старыми модулями не собирается.

## Целевой интерфейс

`combat.hpp`:

```cpp
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
```

`report.hpp`:

```cpp
// Модуль report (после рефакторинга): текстовое описание мира.
#pragma once
#include <cstdint>
#include <string>

#include "world.hpp"

// "0:22 1:dead 2:50" — номер и здоровье или dead для каждого юнита.
std::string describe(const World& w);
// Число живых юнитов.
int32_t alive_count(const World& w);
```

`world.hpp`:

```cpp
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
```
