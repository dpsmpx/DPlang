// t01: заготовка — напишите тела функций по task.md.
#include <cstdio>
#include <optional>
#include <vector>

enum class RingError { None, Full };

struct Ring {
    // поля — на ваш выбор
    std::vector<int> buf;
    int head = 0;  // индекс самого старого элемента
    int len = 0;   // число элементов
    int cap = 0;   // ёмкость
};

Ring ring_new(int capacity) {
    Ring r;
    r.cap = capacity > 0 ? capacity : 0;
    r.buf.assign(static_cast<std::size_t>(r.cap), 0);
    r.head = 0;
    r.len = 0;
    return r;
}

RingError ring_push(Ring& r, int value) {
    if (r.len >= r.cap) {
        return RingError::Full;
    }
    int tail = r.head + r.len;
    if (tail >= r.cap) {
        tail -= r.cap;
    }
    r.buf[static_cast<std::size_t>(tail)] = value;
    r.len += 1;
    return RingError::None;
}

std::optional<int> ring_pop(Ring& r) {
    if (r.len <= 0) {
        return std::nullopt;
    }
    int value = r.buf[static_cast<std::size_t>(r.head)];
    r.head += 1;
    if (r.head >= r.cap) {
        r.head = 0;
    }
    r.len -= 1;
    return value;
}

int ring_len(const Ring& r) {
    return r.len;
}
