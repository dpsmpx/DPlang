// Модуль cart (до рефакторинга): корзина.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "catalog.hpp"

// Строка корзины i — товар items[i] в количестве qtys[i]; строки — в
// порядке первого добавления.
struct Cart {
    std::vector<int32_t> items;
    std::vector<int32_t> qtys;
    int32_t discount = 0;  // скидка в процентах
};

// Кладёт qty штук товара code (к его строке, если она есть).
// Код: 0 — ок; -1 — нет такого товара; -2 — qty < 1.
int32_t cart_add(Cart& cart, const Catalog& c, const std::string& code, int32_t qty);
// Убирает qty штук; строка с 0 штук удаляется.
// Код: 0 — ок; -1 — товара нет в корзине; -2 — qty < 1 или больше, чем лежит.
int32_t cart_remove(Cart& cart, const Catalog& c, const std::string& code, int32_t qty);
// Скидка от 0 до 50 процентов; false — вне диапазона (скидка не меняется).
bool cart_discount(Cart& cart, int32_t percent);
// Сумма строк price * qty минус скидка: (сумма * процент) / 100 с
// отбрасыванием дробной части.
int64_t cart_total(const Cart& cart, const Catalog& c);
