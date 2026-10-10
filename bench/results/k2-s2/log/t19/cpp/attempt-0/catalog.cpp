#include "catalog.hpp"

int32_t catalog_add(Catalog& c, const std::string& code, int64_t price) {
    if (catalog_find(c, code) >= 0) return -1;
    c.items.push_back(Item{code, price});
    return (int32_t)c.items.size() - 1;
}

int32_t catalog_find(const Catalog& c, const std::string& code) {
    for (size_t i = 0; i < c.items.size(); i++)
        if (c.items[i].code == code) return (int32_t)i;
    return -1;
}

int64_t catalog_price(const Catalog& c, int32_t item) { return c.items[item].price; }

std::string catalog_code(const Catalog& c, int32_t item) { return c.items[item].code; }
