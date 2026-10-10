#include "receipt.hpp"

#include "money.hpp"

std::string receipt(const Cart& cart, const Catalog& c) {
    if (cart.items.empty()) return "empty";
    std::string out;
    int64_t sum = 0;
    for (size_t i = 0; i < cart.items.size(); i++) {
        int64_t line = catalog_price(c, cart.items[i]) * cart.qtys[i];
        sum += line;
        out += catalog_code(c, cart.items[i]) + " x" + std::to_string(cart.qtys[i]) + " " + fmt_money(line) + "; ";
    }
    int64_t total = cart_total(cart, c);
    if (cart.discount != 0) out += "discount " + std::to_string(cart.discount) + "% " + fmt_money(total - sum) + "; ";
    return out + "total " + fmt_money(total);
}
