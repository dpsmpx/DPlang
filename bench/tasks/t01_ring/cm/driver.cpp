// Драйвер t01 для переведённых вариантов DPlang (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void push(Ring& r, int32_t v) {
    cm::Result<void> res = ring_push(r, v);
    std::printf("push %d %s\n", v, res.ok() ? "ok" : cm::err_is(res, "Full") ? "full" : "?");
}
static void pop(Ring& r) {
    std::optional<int32_t> v = ring_pop(r);
    if (v) std::printf("pop %d\n", *v); else std::printf("pop none\n");
}

int main() {
    Ring r = ring_new(3);
    push(r, 1); push(r, 2); push(r, 3); push(r, 4);
    std::printf("len %d\n", (int)ring_len(r));
    pop(r); push(r, 4);
    pop(r); pop(r); pop(r); pop(r);
    std::printf("len %d\n", (int)ring_len(r));
    for (int i = 10; i < 17; i++) { push(r, i); if (i % 2) pop(r); }
    std::printf("len %d\n", (int)ring_len(r));
    return 0;
}
