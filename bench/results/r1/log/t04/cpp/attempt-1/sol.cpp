// t04: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <vector>

struct Handle { int index; int generation; };

struct Pool {
    // поля — на ваш выбор
    std::vector<int> values;
    std::vector<int> generations;
    std::vector<bool> alive;
    int live = 0;
};

struct CreateResult { bool full; Handle handle; };

Pool pool_new(int capacity) {
    Pool p;
    int cap = capacity > 0 ? capacity : 0;
    p.values.assign(static_cast<size_t>(cap), 0);
    p.generations.assign(static_cast<size_t>(cap), 1);
    p.alive.assign(static_cast<size_t>(cap), false);
    p.live = 0;
    return p;
}

// Слот хэндла жив и поколение совпадает.
static bool pool_valid(const Pool& p, Handle h) {
    if (h.index < 0 || static_cast<size_t>(h.index) >= p.alive.size()) {
        return false;
    }
    size_t i = static_cast<size_t>(h.index);
    return p.alive[i] && p.generations[i] == h.generation;
}

CreateResult pool_create(Pool& p, int value) {
    for (size_t i = 0; i < p.alive.size(); ++i) {
        if (!p.alive[i]) {
            p.alive[i] = true;
            p.values[i] = value;
            p.live += 1;
            return CreateResult{false, Handle{static_cast<int>(i), p.generations[i]}};
        }
    }
    return CreateResult{true, Handle{-1, 0}};
}

std::optional<int> pool_get(Pool& p, Handle h) {
    if (!pool_valid(p, h)) {
        return std::nullopt;
    }
    return p.values[static_cast<size_t>(h.index)];
}

bool pool_set(Pool& p, Handle h, int value) {
    if (!pool_valid(p, h)) {
        return false;
    }
    p.values[static_cast<size_t>(h.index)] = value;
    return true;
}

bool pool_destroy(Pool& p, Handle h) {
    if (!pool_valid(p, h)) {
        return false;
    }
    size_t i = static_cast<size_t>(h.index);
    p.alive[i] = false;
    p.values[i] = 0;
    // Поколение растёт на 1 при каждом удалении; переполнение без UB.
    p.generations[i] = static_cast<int>(static_cast<unsigned>(p.generations[i]) + 1u);
    p.live -= 1;
    return true;
}

int pool_count(const Pool& p) {
    return p.live;
}
