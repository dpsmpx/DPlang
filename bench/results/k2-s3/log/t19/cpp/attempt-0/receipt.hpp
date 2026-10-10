// Модуль receipt (до рефакторинга): чек.
#pragma once
#include <string>

#include "cart.hpp"
#include "catalog.hpp"

// Чек: по строке на строку корзины "CODE xQTY SUM"; если скидка не 0 —
// "discount P% -X"; затем "total T". Пустая корзина — "empty".
// Строки чека разделены "; ".
std::string receipt(const Cart& cart, const Catalog& c);
