// Эталон М2 на C++ (-O2): slot map на std::vector — значения, поколения,
// признак жизни и стек свободных номеров; тот же кадр и итог, что у
// bench/ecs/pool.dpm.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

struct Handle {
    uint32_t index, gen;
};

struct Body {
    int64_t x, y, vx, vy, hp;
    std::optional<Handle> target;
};

template <class T>
struct SlotMap {
    std::vector<T> vals;
    std::vector<uint32_t> gens;
    std::vector<uint8_t> live;
    std::vector<uint32_t> free;
    int64_t count = 0;

    bool valid(Handle h) const { return h.index < vals.size() && live[h.index] && gens[h.index] == h.gen; }
    T* get(Handle h) { return valid(h) ? &vals[h.index] : nullptr; }
    Handle add(const T& v) {
        count++;
        if (!free.empty()) {
            uint32_t i = free.back();
            free.pop_back();
            vals[i] = v;
            live[i] = 1;
            return {i, gens[i]};
        }
        vals.push_back(v);
        gens.push_back(1);
        live.push_back(1);
        return {uint32_t(vals.size() - 1), 1};
    }
    void remove(Handle h) {
        if (!valid(h)) return;
        live[h.index] = 0;
        gens[h.index]++;
        free.push_back(h.index);
        count--;
    }
};

static uint64_t mix(uint64_t x) {
    uint64_t h = x * 11400714819323198485ull;
    h = (h ^ (h >> 31)) * 13787848793156543929ull;
    return h ^ (h >> 29);
}

static Body body(uint64_t seed, const std::vector<Handle>& all) {
    Body b{int64_t(seed % 1000000), int64_t((seed >> 20) % 1000000), int64_t(seed % 201) - 100,
           int64_t((seed >> 8) % 201) - 100, 50 + int64_t((seed >> 16) % 50), std::nullopt};
    if (!all.empty()) b.target = all[seed % all.size()];
    return b;
}

int main(int argc, char** argv) {
    int64_t n = argc > 1 ? atoll(argv[1]) : 100000, frames = argc > 2 ? atoll(argv[2]) : 100;
    SlotMap<Body> bodies;
    std::vector<Handle> all;
    uint64_t seed = 1;
    for (int64_t i = 0; i < n; i++) {
        seed = mix(seed + uint64_t(i));
        all.push_back(bodies.add(body(seed, all)));
    }
    int64_t born = 0;
    for (int64_t f = 0; f < frames; f++) {
        for (size_t i = 0; i < bodies.vals.size(); i++) {
            if (!bodies.live[i]) continue;
            Body& b = bodies.vals[i];
            b.x += b.vx;
            if (b.x < 0 || b.x > 1000000) {
                b.vx = -b.vx;
                b.x += 2 * b.vx;
            }
            b.y += b.vy;
            if (b.y < 0 || b.y > 1000000) {
                b.vy = -b.vy;
                b.y += 2 * b.vy;
            }
        }
        for (size_t i = 0; i < all.size(); i++) {
            Body& b = *bodies.get(all[i]);
            if (!b.target) continue;
            if (Body* t = bodies.get(*b.target)) t->hp -= 1;
            else b.target.reset();
        }
        int64_t dead = 0;
        for (size_t i = 0; i < all.size();) {
            if (bodies.get(all[i])->hp <= 0) {
                bodies.remove(all[i]);
                all[i] = all.back();
                all.pop_back();
                dead++;
            } else {
                i++;
            }
        }
        for (int64_t k = 0; k < dead; k++) {
            seed = mix(seed + uint64_t(f * 7 + k));
            all.push_back(bodies.add(body(seed, all)));
            born++;
        }
    }
    int64_t sum = 0;
    for (size_t i = 0; i < bodies.vals.size(); i++) {
        if (bodies.live[i]) sum += bodies.vals[i].x + bodies.vals[i].y + bodies.vals[i].hp;
    }
    printf("%lld %lld %lld\n", (long long)bodies.count, (long long)born, (long long)sum);
}
