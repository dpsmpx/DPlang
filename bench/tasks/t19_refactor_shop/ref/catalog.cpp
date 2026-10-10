#include "catalog.hpp"

AddItemResult catalog_add(Catalog& c, const std::string& code, int64_t price) {
    if (catalog_find(c, code)) return {CatalogError::Duplicate, ItemId{0}};
    c.items.push_back(Item{code, price});
    return {CatalogError::None, ItemId{(int32_t)c.items.size() - 1}};
}

std::optional<ItemId> catalog_find(const Catalog& c, const std::string& code) {
    for (size_t i = 0; i < c.items.size(); i++)
        if (c.items[i].code == code) return ItemId{(int32_t)i};
    return std::nullopt;
}

int64_t catalog_price(const Catalog& c, ItemId id) { return c.items[id.index].price; }

std::string catalog_code(const Catalog& c, ItemId id) { return c.items[id.index].code; }
