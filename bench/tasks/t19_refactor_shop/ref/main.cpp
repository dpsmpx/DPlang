// Драйвер после рефакторинга (новый интерфейс). Вывод — тот же, что до.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "cart.hpp"
#include "catalog.hpp"
#include "money.hpp"
#include "receipt.hpp"

static std::string joined(const Cart& cart, const Catalog& c) {
    std::string out;
    for (const std::string& line : receipt(cart, c)) {
        if (!out.empty()) out += "; ";
        out += line;
    }
    return out;
}

static const char* error_text(CartError e) {
    switch (e) {
        case CartError::None: return "ok";
        case CartError::UnknownItem: return "unknown item";
        case CartError::NotInCart: return "not in cart";
        case CartError::BadQuantity: return "bad quantity";
        case CartError::BadDiscount: return "bad discount";
    }
    return "?";
}

static void price(const char* s) {
    MoneyResult r = parse_money(s);
    if (r.error == MoneyError::BadMoney) std::printf("price %s: bad\n", s);
    else std::printf("price %s: %s\n", s, fmt_money(r.cents).c_str());
}

static void add_item(Catalog& c, const char* code, const char* p) {
    AddItemResult r = catalog_add(c, code, parse_money(p).cents);
    if (r.error == CatalogError::Duplicate) std::printf("catalog %s: duplicate\n", code);
    else std::printf("catalog %s: item %d\n", code, r.id.index);
}

static void find(const Catalog& c, const char* code) {
    std::optional<ItemId> id = catalog_find(c, code);
    if (!id) std::printf("find %s: none\n", code);
    else std::printf("find %s: %s at %s\n", code, catalog_code(c, *id).c_str(), fmt_money(catalog_price(c, *id)).c_str());
}

static void add(Cart& cart, const Catalog& c, const char* code, int32_t qty) {
    CartError e = cart_add(cart, c, code, qty);
    std::printf("add %s %d: %s | %s\n", code, qty, error_text(e), joined(cart, c).c_str());
}

static void remove(Cart& cart, const Catalog& c, const char* code, int32_t qty) {
    CartError e = cart_remove(cart, c, code, qty);
    std::printf("remove %s %d: %s | %s\n", code, qty, error_text(e), joined(cart, c).c_str());
}

static void discount(Cart& cart, const Catalog& c, int32_t percent) {
    CartError e = cart_discount(cart, percent);
    std::printf("discount %d: %s | total %s\n", percent, error_text(e), fmt_money(receipt_total(cart, c)).c_str());
}

int main() {
    for (const char* s : {"0.45", "12.00", "007.10", "3.5", "1.234", "-1.00", ".99", "1234567890123.00", "123456789012.99", "1,00", ""})
        price(s);
    Catalog c;
    add_item(c, "apple", "0.45");
    add_item(c, "bread", "2.10");
    add_item(c, "cheese", "7.99");
    add_item(c, "apple", "9.99");
    add_item(c, "gum", "0.05");
    find(c, "cheese");
    find(c, "milk");
    Cart cart;
    std::printf("%s\n", joined(cart, c).c_str());
    add(cart, c, "apple", 3);
    add(cart, c, "bread", 1);
    add(cart, c, "milk", 1);
    add(cart, c, "milk", 0);
    add(cart, c, "cheese", 0);
    add(cart, c, "cheese", 2);
    add(cart, c, "apple", 2);
    remove(cart, c, "bread", 2);
    remove(cart, c, "milk", 1);
    remove(cart, c, "gum", 1);
    remove(cart, c, "apple", 0);
    discount(cart, c, 60);
    discount(cart, c, -1);
    discount(cart, c, 10);
    remove(cart, c, "bread", 1);
    add(cart, c, "bread", 4);
    remove(cart, c, "apple", 5);
    discount(cart, c, 50);
    Cart small;
    add(small, c, "gum", 1);
    discount(small, c, 50);
    std::printf("%s\n", joined(small, c).c_str());
    discount(small, c, 0);
    remove(small, c, "gum", 1);
    return 0;
}
