#include "cart.hpp"

static int32_t line_of(const Cart& cart, ItemId item) {
    for (size_t i = 0; i < cart.lines.size(); i++)
        if (cart.lines[i].item.index == item.index) return (int32_t)i;
    return -1;
}

CartError cart_add(Cart& cart, const Catalog& c, const std::string& code, int32_t qty) {
    std::optional<ItemId> item = catalog_find(c, code);
    if (!item.has_value()) return CartError::UnknownItem;
    if (qty < 1) return CartError::BadQuantity;
    int32_t line = line_of(cart, *item);
    if (line >= 0) {
        cart.lines[line].qty += qty;
    } else {
        cart.lines.push_back(CartLine{*item, qty});
    }
    return CartError::None;
}

CartError cart_remove(Cart& cart, const Catalog& c, const std::string& code, int32_t qty) {
    std::optional<ItemId> item = catalog_find(c, code);
    int32_t line = !item.has_value() ? -1 : line_of(cart, *item);
    if (line < 0) return CartError::NotInCart;
    if (qty < 1 || qty > cart.lines[line].qty) return CartError::BadQuantity;
    cart.lines[line].qty -= qty;
    if (cart.lines[line].qty == 0) cart.lines.erase(cart.lines.begin() + line);
    return CartError::None;
}

CartError cart_discount(Cart& cart, int32_t percent) {
    if (percent < 0 || percent > 50) return CartError::BadDiscount;
    cart.discount_percent = percent;
    return CartError::None;
}
