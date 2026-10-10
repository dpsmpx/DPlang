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
