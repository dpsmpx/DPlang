// Эталон t04 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdio>
#include <optional>
#include <vector>

struct Handle { int index; int generation; };

struct Slot { int generation = 1; bool alive = false; int value = 0; };

struct Pool { std::vector<Slot> slots; int count = 0; };

struct CreateResult { bool full; Handle handle; };

Pool pool_new(int capacity) {
    Pool p;
    p.slots.resize(capacity);
    return p;
}

CreateResult pool_create(Pool& p, int value) {
    for (int i = 0; i < (int)p.slots.size(); i++) {
        Slot& s = p.slots[i];
        if (!s.alive) {
            s.alive = true;
            s.value = value;
            p.count++;
            return {false, {i, s.generation}};
        }
    }
    return {true, {-1, 0}};
}

static Slot* live(Pool& p, Handle h) {
    if (h.index < 0 || h.index >= (int)p.slots.size()) return nullptr;
    Slot& s = p.slots[h.index];
    return (s.alive && s.generation == h.generation) ? &s : nullptr;
}

std::optional<int> pool_get(Pool& p, Handle h) {
    Slot* s = live(p, h);
    if (!s) return std::nullopt;
    return s->value;
}

bool pool_set(Pool& p, Handle h, int value) {
    Slot* s = live(p, h);
    if (!s) return false;
    s->value = value;
    return true;
}

bool pool_destroy(Pool& p, Handle h) {
    Slot* s = live(p, h);
    if (!s) return false;
    s->alive = false;
    s->generation++;
    p.count--;
    return true;
}

int pool_count(const Pool& p) { return p.count; }

// --- driver ---
static void show(const char* name, Handle h) { std::printf("%s = (%d,%d)\n", name, h.index, h.generation); }
static void get(Pool& p, const char* name, Handle h) {
    auto v = pool_get(p, h);
    if (v) std::printf("get %s %d\n", name, *v); else std::printf("get %s stale\n", name);
}

int main() {
    Pool p = pool_new(3);
    Handle a = pool_create(p, 10).handle; show("a", a);
    Handle b = pool_create(p, 20).handle; show("b", b);
    Handle c = pool_create(p, 30).handle; show("c", c);
    std::printf("create full %d\n", pool_create(p, 40).full ? 1 : 0);
    std::printf("destroy b %d\n", pool_destroy(p, b) ? 1 : 0);
    std::printf("destroy b %d\n", pool_destroy(p, b) ? 1 : 0);
    get(p, "b", b);
    Handle d = pool_create(p, 50).handle; show("d", d);
    get(p, "b", b);
    get(p, "d", d);
    std::printf("set b %d\n", pool_set(p, b, 1) ? 1 : 0);
    std::printf("set d %d\n", pool_set(p, d, 55) ? 1 : 0);
    get(p, "d", d);
    std::printf("count %d\n", pool_count(p));
    std::printf("destroy a %d\n", pool_destroy(p, a) ? 1 : 0);
    std::printf("destroy c %d\n", pool_destroy(p, c) ? 1 : 0);
    Handle e = pool_create(p, 60).handle; show("e", e);
    get(p, "a", a);
    get(p, "e", e);
    std::printf("count %d\n", pool_count(p));
    get(p, "bad", Handle{7, 1});
    return 0;
}
