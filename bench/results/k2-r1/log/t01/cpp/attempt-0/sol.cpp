// t01: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <vector>

enum class RingError { None, Full };

struct Ring {
    // поля — на ваш выбор
};

Ring ring_new(int capacity) {
    // ...
}

RingError ring_push(Ring& r, int value) {
    // ...
}

std::optional<int> ring_pop(Ring& r) {
    // ...
}

int ring_len(const Ring& r) {
    // ...
}
