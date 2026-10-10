// Модуль catalog (до рефакторинга): товары по номерам.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Item {
    std::string code;
    int64_t price;  // копейки
};

struct Catalog {
    std::vector<Item> items;  // номер товара — индекс
};

// Добавляет товар; его номер или -1, если код уже есть.
int32_t catalog_add(Catalog& c, const std::string& code, int64_t price);
// Номер товара по коду или -1.
int32_t catalog_find(const Catalog& c, const std::string& code);
int64_t catalog_price(const Catalog& c, int32_t item);
std::string catalog_code(const Catalog& c, int32_t item);
