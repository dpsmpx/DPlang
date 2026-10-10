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
