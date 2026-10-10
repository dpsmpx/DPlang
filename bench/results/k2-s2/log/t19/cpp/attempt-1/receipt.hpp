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
