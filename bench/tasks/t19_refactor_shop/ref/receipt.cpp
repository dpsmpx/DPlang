#include "receipt.hpp"

#include "money.hpp"

int64_t receipt_total(const Cart& cart, const Catalog& c) {
    int64_t sum = 0;
    for (const CartLine& l : cart.lines) sum += catalog_price(c, l.item) * l.qty;
    return sum - sum * cart.discount_percent / 100;
}

std::vector<std::string> receipt(const Cart& cart, const Catalog& c) {
    if (cart.lines.empty()) return {"empty"};
    std::vector<std::string> out;
    int64_t sum = 0;
    for (const CartLine& l : cart.lines) {
        int64_t line = catalog_price(c, l.item) * l.qty;
        sum += line;
        out.push_back(catalog_code(c, l.item) + " x" + std::to_string(l.qty) + " " + fmt_money(line));
    }
    int64_t total = receipt_total(cart, c);
    if (cart.discount_percent != 0)
        out.push_back("discount " + std::to_string(cart.discount_percent) + "% " + fmt_money(total - sum));
    out.push_back("total " + fmt_money(total));
    return out;
}
