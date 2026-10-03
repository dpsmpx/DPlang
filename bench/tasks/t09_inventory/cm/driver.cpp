// Драйвер t09 для переведённых вариантов DPlang (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void add(Inventory& inv, int32_t item, int32_t count) {
    cm::Result<int32_t> r = inv_add(inv, item, count);
    if (!r.ok()) std::printf("add %d %d invalid\n", item, count);
    else std::printf("add %d %d left %d | %s\n", item, count, r.value, inv_describe(inv).c_str());
}
static void rem(Inventory& inv, int32_t item, int32_t count) {
    cm::Result<void> r = inv_remove(inv, item, count);
    const char* t = r.ok() ? "ok" : cm::err_is(r, "NotEnough") ? "not_enough" : "invalid";
    std::printf("remove %d %d %s | %s\n", item, count, t, inv_describe(inv).c_str());
}

int main() {
    Inventory inv = inv_new(4);
    add(inv, 1, 50);
    add(inv, 1, 30);
    add(inv, 200, 2);
    add(inv, 2, 100);
    add(inv, 0, 1);
    add(inv, 3, 0);
    rem(inv, 1, 70);
    rem(inv, 1, 20);
    rem(inv, 200, 1);
    add(inv, 2, 70);
    std::printf("total 2 = %d, total 1 = %d\n", inv_total(inv, 2), inv_total(inv, 1));
    return 0;
}
