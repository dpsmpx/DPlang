// Эталон замера «большие данные» (заметка 11): тот же генератор мира,
// кодирование и чтение, что у world.dpm; вывод должен совпасть.
//
//     world <g|ge|encode|roundtrip> <чанков по стороне> <зерно>
//     world write <чанков по стороне> <зерно> <файл>    — encode и запись файла по чанкам
//     world write1 <чанков по стороне> <зерно> <файл>   — то же одним буфером: склейка, одна запись
//     world readback <чанков по стороне> <зерно> <файл> — g, чтение файла, сверка и хэш
//
// Потоки — DPL_THREADS (как у runtime dpl) или число процессоров; чанки
// делятся на равные полосы.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static const int64_t SIDE = 64;
static const int64_t TILES = 4096;

struct Obj {
    uint8_t kind, x, y;
    uint32_t value;
};

struct Chunk {
    int32_t cx = 0, cy = 0;
    std::vector<uint16_t> heights;
    std::vector<uint8_t> biomes;
    std::vector<Obj> objects;
};

static uint64_t mix(uint64_t seed, int64_t a, int64_t b) {
    uint64_t z = seed ^ ((uint64_t)a * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)b * 0xD1B54A32D192ED03ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static std::vector<int64_t> lattice(uint64_t seed, int64_t cx, int64_t cy, int64_t lg, int64_t layer) {
    int64_t n = SIDE >> lg;
    std::vector<int64_t> g;
    g.reserve((n + 1) * (n + 1));
    for (int64_t j = 0; j < n + 1; j++)
        for (int64_t i = 0; i < n + 1; i++) g.push_back((int64_t)(mix(seed ^ (uint64_t)layer, cx * n + i, cy * n + j) & 0xFFFF));
    return g;
}

static int64_t sample(const std::vector<int64_t>& g, int64_t lg, int64_t x, int64_t y) {
    int64_t n = SIDE >> lg, s = (int64_t)1 << lg;
    int64_t x0 = x >> lg, y0 = y >> lg, fx = x & (s - 1), fy = y & (s - 1);
    int64_t a = g[y0 * (n + 1) + x0], b = g[y0 * (n + 1) + x0 + 1];
    int64_t c = g[(y0 + 1) * (n + 1) + x0], d = g[(y0 + 1) * (n + 1) + x0 + 1];
    int64_t top = a + (b - a) * fx / s, bot = c + (d - c) * fx / s;
    return top + (bot - top) * fy / s;
}

static Chunk gen_chunk(uint64_t seed, int64_t cx, int64_t cy) {
    auto o6 = lattice(seed, cx, cy, 6, 0), o4 = lattice(seed, cx, cy, 4, 1), o2 = lattice(seed, cx, cy, 2, 2),
         wet = lattice(seed, cx, cy, 5, 3);
    Chunk c;
    c.cx = (int32_t)cx;
    c.cy = (int32_t)cy;
    c.heights.reserve(TILES);
    c.biomes.reserve(TILES);
    for (int64_t y = 0; y < SIDE; y++) {
        for (int64_t x = 0; x < SIDE; x++) {
            int64_t h = (sample(o6, 6, x, y) * 4 + sample(o4, 4, x, y) * 2 + sample(o2, 2, x, y)) / 7;
            int64_t m = sample(wet, 5, x, y);
            uint8_t b = 1;
            if (h < 20000) b = 0;
            else if (h > 52000) b = 4;
            else if (m > 40000) b = 2;
            else if (m < 15000) b = 3;
            c.heights.push_back((uint16_t)h);
            c.biomes.push_back(b);
            uint64_t r = mix(seed ^ 7, cx * SIDE + x, cy * SIDE + y);
            if ((b == 2 && r % 16 == 0) || (b == 1 && r % 64 == 0))
                c.objects.push_back(Obj{b, (uint8_t)x, (uint8_t)y, (uint32_t)(r >> 32)});
        }
    }
    return c;
}

static void put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(v & 255);
    out.push_back(v >> 8);
}

static void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(v & 255);
    out.push_back((v >> 8) & 255);
    out.push_back((v >> 16) & 255);
    out.push_back(v >> 24);
}

static std::vector<uint8_t> encode(const Chunk& c) {
    std::vector<uint8_t> out;
    out.reserve(12 + TILES * 3 + c.objects.size() * 7);
    put32(out, (uint32_t)c.cx);
    put32(out, (uint32_t)c.cy);
    put32(out, (uint32_t)c.objects.size());
    for (uint16_t h : c.heights) put16(out, h);
    for (uint8_t b : c.biomes) out.push_back(b);
    for (const Obj& o : c.objects) {
        out.push_back(o.kind);
        out.push_back(o.x);
        out.push_back(o.y);
        put32(out, o.value);
    }
    return out;
}

static uint16_t get16(const uint8_t* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t get32(const uint8_t* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Чанк из байтов b длины len; false — длина не сходится.
static bool decode(const uint8_t* b, size_t len, Chunk& c) {
    if (len < 12 + TILES * 3) return false;
    uint32_t n = get32(b + 8);
    if (len != 12 + TILES * 3 + (size_t)n * 7) return false;
    c.cx = (int32_t)get32(b);
    c.cy = (int32_t)get32(b + 4);
    c.heights.resize(TILES);
    c.biomes.resize(TILES);
    for (int64_t k = 0; k < TILES; k++) c.heights[k] = get16(b + 12 + k * 2);
    memcpy(c.biomes.data(), b + 12 + TILES * 2, TILES);
    c.objects.resize(n);
    const uint8_t* at = b + 12 + TILES * 3;
    for (uint32_t k = 0; k < n; k++, at += 7) c.objects[k] = Obj{at[0], at[1], at[2], get32(at + 3)};
    return true;
}

static bool same(const Chunk& a, const Chunk& b) {
    if (a.cx != b.cx || a.cy != b.cy || a.heights != b.heights || a.biomes != b.biomes) return false;
    if (a.objects.size() != b.objects.size()) return false;
    for (size_t k = 0; k < a.objects.size(); k++) {
        const Obj &p = a.objects[k], &q = b.objects[k];
        if (p.kind != q.kind || p.x != q.x || p.y != q.y || p.value != q.value) return false;
    }
    return true;
}

static uint64_t fnv(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001B3ull; }

static uint64_t hash_bytes(const uint8_t* b, size_t len) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t k = 0; k < len; k++) h = fnv(h, b[k]);
    return h;
}

template <class F>
static void par_for(int64_t n, F f) {
    int threads = (int)std::thread::hardware_concurrency();
    if (const char* e = getenv("DPL_THREADS")) threads = atoi(e) > 0 ? atoi(e) : threads;
    if (threads < 1) threads = 1;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++) {
        int64_t lo = n * t / threads, hi = n * (t + 1) / threads;
        pool.emplace_back([=, &f] {
            for (int64_t i = lo; i < hi; i++) f(i);
        });
    }
    for (auto& t : pool) t.join();
}

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    bool file_mode = mode == "write" || mode == "write1" || mode == "readback";
    bool plain = mode == "g" || mode == "ge" || mode == "encode" || mode == "roundtrip";
    if (!(plain && argc == 4) && !(file_mode && argc == 5)) {
        fprintf(stderr, "usage: world <g|ge|encode|roundtrip> <chunks per side> <seed> | <write|write1|readback> ... <file>\n");
        return 2;
    }
    int64_t w = atoll(argv[2]);
    uint64_t seed = (uint64_t)atoll(argv[3]);
    int64_t n = w * w;
    std::vector<Chunk> chunks(n);
    par_for(n, [&](int64_t i) { chunks[i] = gen_chunk(seed, i % w, i / w); });
    int64_t objects = 0, total = 0;
    for (const Chunk& c : chunks) objects += (int64_t)c.objects.size();
    std::vector<uint64_t> sums(n, 0);
    if (mode == "readback") {
        FILE* f = fopen(argv[4], "rb");
        if (!f) return 3;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> all((size_t)size);
        if (fread(all.data(), 1, all.size(), f) != all.size()) return 3;
        fclose(f);
        std::vector<size_t> at(n + 1, 0);
        for (int64_t i = 0; i < n; i++) at[i + 1] = at[i] + 12 + TILES * 3 + (size_t)get32(all.data() + at[i] + 8) * 7;
        std::vector<char> ok(n, 0);
        par_for(n, [&](int64_t i) {
            Chunk c;
            ok[i] = decode(all.data() + at[i], at[i + 1] - at[i], c) && same(c, chunks[i]);
            sums[i] = hash_bytes(all.data() + at[i], at[i + 1] - at[i]);
        });
        for (char k : ok)
            if (!k) {
                fprintf(stderr, "readback: a chunk does not match\n");
                return 1;
            }
        total = size;
        mode = "roundtrip";
    } else if (mode != "g") {
        std::vector<std::vector<uint8_t>> blobs(n);
        par_for(n, [&](int64_t i) { blobs[i] = encode(chunks[i]); });
        for (const auto& b : blobs) total += (int64_t)b.size();
        if (mode != "ge") par_for(n, [&](int64_t i) { sums[i] = hash_bytes(blobs[i].data(), blobs[i].size()); });
        if (mode == "roundtrip") {
            std::vector<char> ok(n, 0);
            par_for(n, [&](int64_t i) {
                Chunk c;
                ok[i] = decode(blobs[i].data(), blobs[i].size(), c) && same(c, chunks[i]);
            });
            for (char k : ok)
                if (!k) {
                    fprintf(stderr, "roundtrip: a chunk does not match\n");
                    return 1;
                }
        }
        if (mode == "write" || mode == "write1") {
            FILE* f = fopen(argv[4], "wb");
            if (!f) return 3;
            if (mode == "write") {
                for (const auto& b : blobs)
                    if (fwrite(b.data(), 1, b.size(), f) != b.size()) return 3;
            } else {
                std::vector<uint8_t> all;
                all.reserve((size_t)total);
                for (const auto& b : blobs) all.insert(all.end(), b.begin(), b.end());
                if (fwrite(all.data(), 1, all.size(), f) != all.size()) return 3;
            }
            if (fclose(f) != 0) return 3;
            mode = "encode";
        }
    }
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t s : sums) h = fnv(h, s);
    printf("%s chunks=%lld objects=%lld bytes=%lld hash=%016llx\n", mode.c_str(), (long long)n, (long long)objects,
           (long long)total, (unsigned long long)h);
    return 0;
}
