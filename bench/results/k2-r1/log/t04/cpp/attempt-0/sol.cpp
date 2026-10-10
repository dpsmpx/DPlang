// t04: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <vector>

struct Handle { int index; int generation; };

struct Pool {
    // поля — на ваш выбор
};

struct CreateResult { bool full; Handle handle; };

Pool pool_new(int capacity) {
    // ...
}

CreateResult pool_create(Pool& p, int value) {
    // ...
}

std::optional<int> pool_get(Pool& p, Handle h) {
    // ...
}

bool pool_set(Pool& p, Handle h, int value) {
    // ...
}

bool pool_destroy(Pool& p, Handle h) {
    // ...
}

int pool_count(const Pool& p) {
    // ...
}
