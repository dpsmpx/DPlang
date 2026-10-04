// t01: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <vector>

enum class RingError { None, Full };

struct Ring {
    // поля — на ваш выбор
    std::vector<int> buf;
    int head = 0;
    int len = 0;
    int cap = 0;
};

Ring ring_new(int capacity) {
    Ring r;
    if (capacity < 0) {
        capacity = 0;
    }
    r.cap = capacity;
    r.buf.assign(static_cast<std::size_t>(capacity), 0);
    r.head = 0;
    r.len = 0;
    return r;
}

RingError ring_push(Ring& r, int value) {
    if (r.len >= r.cap) {
        return RingError::Full;
    }
    int tail = (r.head + r.len) % r.cap;
    r.buf[static_cast<std::size_t>(tail)] = value;
    r.len += 1;
    return RingError::None;
}

std::optional<int> ring_pop(Ring& r) {
    if (r.len == 0) {
        return std::nullopt;
    }
    int value = r.buf[static_cast<std::size_t>(r.head)];
    r.head = (r.head + 1) % r.cap;
    r.len -= 1;
    return value;
}

int ring_len(const Ring& r) {
    return r.len;
}
