// t04: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <vector>

struct Handle { int index; int generation; };

struct Slot {
    int value;
    int generation;
    bool alive;
};

struct Pool {
    std::vector<Slot> slots;
    int live;
};

struct CreateResult { bool full; Handle handle; };

static Slot* pool_live_slot(Pool& p, Handle h) {
    if (h.index < 0 || static_cast<size_t>(h.index) >= p.slots.size()) {
        return nullptr;
    }
    Slot& s = p.slots[static_cast<size_t>(h.index)];
    if (!s.alive || s.generation != h.generation) {
        return nullptr;
    }
    return &s;
}

Pool pool_new(int capacity) {
    Pool p;
    p.live = 0;
    if (capacity > 0) {
        p.slots.assign(static_cast<size_t>(capacity), Slot{0, 1, false});
    }
    return p;
}

CreateResult pool_create(Pool& p, int value) {
    for (size_t i = 0; i < p.slots.size(); ++i) {
        Slot& s = p.slots[i];
        if (!s.alive) {
            s.alive = true;
            s.value = value;
            p.live += 1;
            return CreateResult{false, Handle{static_cast<int>(i), s.generation}};
        }
    }
    return CreateResult{true, Handle{-1, 0}};
}

std::optional<int> pool_get(Pool& p, Handle h) {
    Slot* s = pool_live_slot(p, h);
    if (s == nullptr) {
        return std::nullopt;
    }
    return s->value;
}

bool pool_set(Pool& p, Handle h, int value) {
    Slot* s = pool_live_slot(p, h);
    if (s == nullptr) {
        return false;
    }
    s->value = value;
    return true;
}

bool pool_destroy(Pool& p, Handle h) {
    Slot* s = pool_live_slot(p, h);
    if (s == nullptr) {
        return false;
    }
    s->alive = false;
    // Без переполнения со знаком (UB): заворачиваем через беззнаковое.
    s->generation = static_cast<int>(static_cast<unsigned>(s->generation) + 1u);
    p.live -= 1;
    return true;
}

int pool_count(const Pool& p) {
    return p.live;
}
