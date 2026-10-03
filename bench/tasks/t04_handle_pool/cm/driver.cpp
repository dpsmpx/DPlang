// Драйвер t04 для переведённых вариантов C- (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void show(const char* name, Handle h) { std::printf("%s = (%d,%d)\n", name, h.index, h.generation); }
static void get(Pool& p, const char* name, Handle h) {
    std::optional<int32_t> v = pool_get(p, h);
    if (v) std::printf("get %s %d\n", name, *v); else std::printf("get %s stale\n", name);
}
static Handle create(Pool& p, int32_t v) {
    cm::Result<Handle> r = pool_create(p, v);
    if (!r.ok()) cm::trap("pool_create: неожиданная ошибка");
    return r.value;
}

int main() {
    Pool p = pool_new(3);
    Handle a = create(p, 10); show("a", a);
    Handle b = create(p, 20); show("b", b);
    Handle c = create(p, 30); show("c", c);
    std::printf("create full %d\n", cm::err_is(pool_create(p, 40), "Full") ? 1 : 0);
    std::printf("destroy b %d\n", pool_destroy(p, b) ? 1 : 0);
    std::printf("destroy b %d\n", pool_destroy(p, b) ? 1 : 0);
    get(p, "b", b);
    Handle d = create(p, 50); show("d", d);
    get(p, "b", b);
    get(p, "d", d);
    std::printf("set b %d\n", pool_set(p, b, 1) ? 1 : 0);
    std::printf("set d %d\n", pool_set(p, d, 55) ? 1 : 0);
    get(p, "d", d);
    std::printf("count %d\n", pool_count(p));
    std::printf("destroy a %d\n", pool_destroy(p, a) ? 1 : 0);
    std::printf("destroy c %d\n", pool_destroy(p, c) ? 1 : 0);
    Handle e = create(p, 60); show("e", e);
    get(p, "a", a);
    get(p, "e", e);
    std::printf("count %d\n", pool_count(p));
    get(p, "bad", Handle{7, 1});
    return 0;
}
