# Задача t19 на C++

Задание опыта: написать код по постановке.

## Правила

- Работай только в папке `/home/user/C-/build/experiment/k2-r1/t19/cpp`: читай этот файл и `src/`, правь файлы в `src/`. Других файлов не открывай, по диску и в сети не ищи.
- Компилятор и другие программы сам не запускай. Проверка — только командой

      python3 /home/user/C-/bench/experiment.py check /home/user/C-/build/experiment/k2-r1/t19/cpp

  Она собирает его со скрытым драйвером, запускает и сравнивает вывод с ожидаемым; печатает «принято» или первую ошибку.
- Попыток проверки — не больше 5, каждая записывается. Проверяй, когда решение готово целиком, а не для разведки.
- Работа кончается на «принято» или когда попытки кончились. Ответ — одна строка: номер последней попытки и её итог.

## Код

В `src/cart.cpp`, `src/cart.hpp`, `src/catalog.cpp`, `src/catalog.hpp`, `src/money.cpp`, `src/money.hpp`, `src/receipt.cpp`, `src/receipt.hpp` — модули до рефакторинга. Измени их так, чтобы интерфейс стал как в разделе «Целевой интерфейс»; поведение не меняется.

Сборка — `g++ -std=c++20 -O1 -pthread -Wall -Wextra -Werror -fno-exceptions -fno-rtti`, с ASan и UBSan. Драйвер `main.cpp` подключает заголовки модулей и собирается вместе со всеми `src/*.cpp`.

## t19. Многошаговый рефакторинг: магазин из четырёх модулей

Дана программа из четырёх модулей — `money`, `catalog`, `cart`,
`receipt` (начальный код задачи). Нужно изменить интерфейс модулей так,
чтобы собирался и проходил **новый драйвер**; целевой интерфейс модулей
приложен к заданию. Поведение не меняется: вывод нового драйвера
совпадает с выводом старого. Мера задачи — точность согласованной правки
в нескольких модулях в несколько шагов.

### Что изменить

1. Номер товара — отдельный тип `ItemId { index: i32 }` вместо голого
   `i32` во всём интерфейсе `catalog` и в корзине.
2. `catalog_add(c, code, price) -> ошибка Duplicate | ItemId` вместо
   номера или `-1`; `catalog_find(c, code) -> optional ItemId` вместо
   `-1`; `catalog_price` и `catalog_code` принимают `ItemId`.
3. `parse_money(s) -> ошибка BadMoney | i64` вместо `-1`.
4. Корзина: две параллельные последовательности `items` и `qtys` —
   одна последовательность строк `lines` из `CartLine { item: ItemId,
   qty: i32 }`; поле `discount` переименовать в `discount_percent`.
5. `cart_add`, `cart_remove` и `cart_discount` возвращают
   `ошибка … | ok` с одним перечислением ошибок корзины `UnknownItem |
   NotInCart | BadQuantity | BadDiscount` вместо кодов `0` / `-1` / `-2`
   и `bool`: код `-1` — `UnknownItem` у `cart_add` и `NotInCart` у
   `cart_remove`, код `-2` — `BadQuantity`, `false` — `BadDiscount`.
6. `cart_total` перенести из `cart` в `receipt` под именем
   `receipt_total`, с тем же смыслом.
7. `receipt(cart, c)` возвращает список строк чека вместо одной строки,
   где они разделены `"; "`; у пустой корзины — список из одной строки
   `"empty"`.

Прочее (запись и разбор сумм, правила корзины и скидки, порядок строк) —
без изменений.

### Проверки

Драйвер — `expected.txt`; старый драйвер со старыми модулями даёт тот же
вывод. Новый драйвер со старыми модулями не собирается.

## Целевой интерфейс

`cart.hpp`:

```cpp
// Модуль cart (после рефакторинга): корзина.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "catalog.hpp"

struct CartLine {
    ItemId item;
    int32_t qty;
};

// Строки — в порядке первого добавления.
struct Cart {
    std::vector<CartLine> lines;
    int32_t discount_percent = 0;
};

enum class CartError { None, UnknownItem, NotInCart, BadQuantity, BadDiscount };

// Кладёт qty штук товара code (к его строке, если она есть).
// Ошибки: UnknownItem — нет такого товара; BadQuantity — qty < 1.
CartError cart_add(Cart& cart, const Catalog& c, const std::string& code, int32_t qty);
// Убирает qty штук; строка с 0 штук удаляется.
// Ошибки: NotInCart — товара нет в корзине; BadQuantity — qty < 1 или больше, чем лежит.
CartError cart_remove(Cart& cart, const Catalog& c, const std::string& code, int32_t qty);
// Скидка от 0 до 50 процентов; иначе ошибка BadDiscount (скидка не меняется).
CartError cart_discount(Cart& cart, int32_t percent);
```

`catalog.hpp`:

```cpp
// Модуль catalog (после рефакторинга): товары по номерам.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct ItemId { int32_t index; };

struct Item {
    std::string code;
    int64_t price;  // копейки
};

struct Catalog {
    std::vector<Item> items;  // ItemId.index — индекс
};

enum class CatalogError { None, Duplicate };
struct AddItemResult { CatalogError error; ItemId id; };

// Добавляет товар; ошибка Duplicate, если код уже есть.
AddItemResult catalog_add(Catalog& c, const std::string& code, int64_t price);
// Товар по коду; нет значения, если такого кода нет.
std::optional<ItemId> catalog_find(const Catalog& c, const std::string& code);
int64_t catalog_price(const Catalog& c, ItemId id);
std::string catalog_code(const Catalog& c, ItemId id);
```

`money.hpp`:

```cpp
// Модуль money (после рефакторинга): суммы в копейках.
#pragma once
#include <cstdint>
#include <string>

enum class MoneyError { None, BadMoney };
struct MoneyResult { MoneyError error; int64_t cents; };

// "12.05", "-0.50", "3.00".
std::string fmt_money(int64_t cents);
// Разбор "D.DD": от 1 до 12 цифр, точка, ровно две цифры; без знака.
MoneyResult parse_money(const std::string& s);
```

`receipt.hpp`:

```cpp
// Модуль receipt (после рефакторинга): итог и чек.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cart.hpp"
#include "catalog.hpp"

// Сумма строк price * qty минус скидка: (сумма * процент) / 100 с
// отбрасыванием дробной части.
int64_t receipt_total(const Cart& cart, const Catalog& c);
// Строки чека: по строке на строку корзины "CODE xQTY SUM"; если скидка
// не 0 — "discount P% -X"; затем "total T". Пустая корзина — одна строка
// "empty".
std::vector<std::string> receipt(const Cart& cart, const Catalog& c);
```
