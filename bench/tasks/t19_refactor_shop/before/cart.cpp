#include "cart.hpp"

static int32_t line_of(const Cart& cart, int32_t item) {
    for (size_t i = 0; i < cart.items.size(); i++)
        if (cart.items[i] == item) return (int32_t)i;
    return -1;
}

int32_t cart_add(Cart& cart, const Catalog& c, const std::string& code, int32_t qty) {
    int32_t item = catalog_find(c, code);
    if (item < 0) return -1;
    if (qty < 1) return -2;
    int32_t line = line_of(cart, item);
    if (line >= 0) {
        cart.qtys[line] += qty;
    } else {
        cart.items.push_back(item);
        cart.qtys.push_back(qty);
    }
    return 0;
}

int32_t cart_remove(Cart& cart, const Catalog& c, const std::string& code, int32_t qty) {
    int32_t item = catalog_find(c, code);
    int32_t line = item < 0 ? -1 : line_of(cart, item);
    if (line < 0) return -1;
    if (qty < 1 || qty > cart.qtys[line]) return -2;
    cart.qtys[line] -= qty;
    if (cart.qtys[line] == 0) {
        cart.items.erase(cart.items.begin() + line);
        cart.qtys.erase(cart.qtys.begin() + line);
    }
    return 0;
}

bool cart_discount(Cart& cart, int32_t percent) {
    if (percent < 0 || percent > 50) return false;
    cart.discount = percent;
    return true;
}

int64_t cart_total(const Cart& cart, const Catalog& c) {
    int64_t sum = 0;
    for (size_t i = 0; i < cart.items.size(); i++) sum += catalog_price(c, cart.items[i]) * cart.qtys[i];
    return sum - sum * cart.discount / 100;
}
