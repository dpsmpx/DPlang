// Эталон t01 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdio>
#include <optional>
#include <vector>

enum class RingError { None, Full };

struct Ring {
    std::vector<int> data;
    int head = 0;
    int count = 0;
};

Ring ring_new(int capacity) {
    Ring r;
    r.data.assign(capacity, 0);
    return r;
}

RingError ring_push(Ring& r, int value) {
    int cap = (int)r.data.size();
    if (r.count == cap) return RingError::Full;
    r.data[(r.head + r.count) % cap] = value;
    r.count++;
    return RingError::None;
}

std::optional<int> ring_pop(Ring& r) {
    if (r.count == 0) return std::nullopt;
    int v = r.data[r.head];
    r.head = (r.head + 1) % (int)r.data.size();
    r.count--;
    return v;
}

int ring_len(const Ring& r) { return r.count; }

// --- driver ---
static void push(Ring& r, int v) {
    std::printf("push %d %s\n", v, ring_push(r, v) == RingError::Full ? "full" : "ok");
}
static void pop(Ring& r) {
    auto v = ring_pop(r);
    if (v) std::printf("pop %d\n", *v); else std::printf("pop none\n");
}

int main() {
    Ring r = ring_new(3);
    push(r, 1); push(r, 2); push(r, 3); push(r, 4);
    std::printf("len %d\n", ring_len(r));
    pop(r); push(r, 4);
    pop(r); pop(r); pop(r); pop(r);
    std::printf("len %d\n", ring_len(r));
    for (int i = 10; i < 17; i++) { push(r, i); if (i % 2) pop(r); }
    std::printf("len %d\n", ring_len(r));
    return 0;
}
