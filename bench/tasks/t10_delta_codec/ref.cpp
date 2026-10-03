// Эталон t10 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdint>
#include <cstdio>
#include <vector>

struct Edit { int32_t x, y, z, block; };
enum class DecodeError { None, Truncated, BadValue, TrailingBytes };
struct DecodeResult { DecodeError error; std::vector<Edit> edits; };

static void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) { out.push_back((uint8_t)(v | 0x80)); v >>= 7; }
    out.push_back((uint8_t)v);
}
static uint64_t zigzag(int64_t n) { return ((uint64_t)n << 1) ^ (uint64_t)(n >> 63); }
static int64_t unzigzag(uint64_t u) { return (int64_t)(u >> 1) ^ -(int64_t)(u & 1); }

std::vector<uint8_t> encode(const std::vector<Edit>& edits) {
    std::vector<uint8_t> out;
    put_varint(out, edits.size());
    int64_t px = 0, py = 0, pz = 0;
    for (const Edit& e : edits) {
        put_varint(out, zigzag((int64_t)e.x - px));
        put_varint(out, zigzag((int64_t)e.y - py));
        put_varint(out, zigzag((int64_t)e.z - pz));
        put_varint(out, (uint64_t)e.block);
        px = e.x; py = e.y; pz = e.z;
    }
    return out;
}

// Читает varint; возвращает ошибку или значение.
static DecodeError get_varint(const std::vector<uint8_t>& in, size_t& pos, uint64_t& v) {
    v = 0;
    for (int i = 0; i < 10; i++) {
        if (pos >= in.size()) return DecodeError::Truncated;
        uint8_t b = in[pos++];
        v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) return DecodeError::None;
    }
    return DecodeError::BadValue;
}

DecodeResult decode(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    uint64_t n;
    if (DecodeError e = get_varint(in, pos, n); e != DecodeError::None) return {e, {}};
    std::vector<Edit> out;
    int64_t px = 0, py = 0, pz = 0;
    for (uint64_t k = 0; k < n; k++) {
        uint64_t u[4];
        for (int f = 0; f < 4; f++)
            if (DecodeError e = get_varint(in, pos, u[f]); e != DecodeError::None) return {e, {}};
        int64_t x = px + unzigzag(u[0]), y = py + unzigzag(u[1]), z = pz + unzigzag(u[2]);
        auto in_i32 = [](int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; };
        if (!in_i32(x) || !in_i32(y) || !in_i32(z) || u[3] > 65535) return {DecodeError::BadValue, {}};
        out.push_back({(int32_t)x, (int32_t)y, (int32_t)z, (int32_t)u[3]});
        px = x; py = y; pz = z;
    }
    if (pos != in.size()) return {DecodeError::TrailingBytes, {}};
    return {DecodeError::None, out};
}

// --- driver ---
static const char* err_name(DecodeError e) {
    switch (e) {
        case DecodeError::Truncated: return "Truncated";
        case DecodeError::BadValue: return "BadValue";
        case DecodeError::TrailingBytes: return "TrailingBytes";
        default: return "None";
    }
}

static void hex(const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < b.size(); i++) std::printf(i ? " %02x" : "%02x", b[i]);
    std::printf("\n");
}

static void dec(const std::vector<uint8_t>& b) {
    DecodeResult r = decode(b);
    if (r.error != DecodeError::None) { std::printf("decode error %s\n", err_name(r.error)); return; }
    std::printf("decode %d:", (int)r.edits.size());
    for (const Edit& e : r.edits) std::printf(" (%d,%d,%d,%d)", e.x, e.y, e.z, e.block);
    std::printf("\n");
}

int main() {
    std::vector<Edit> edits = {{0, 0, 0, 1}, {1, 0, 0, 1}, {1, 64, -3, 300},
                               {-100000, 64, -3, 65535}, {2147483647, -2147483647 - 1, 0, 0}};
    std::vector<uint8_t> bytes = encode(edits);
    hex(bytes);
    dec(bytes);
    hex(encode({}));
    dec(encode({}));
    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
    dec(cut);
    std::vector<uint8_t> extra = bytes; extra.push_back(0);
    dec(extra);
    dec({1, 0, 0, 0, 0x80, 0x80, 0x04});        // block = 65536
    dec({1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01});
    dec({});
    return 0;
}
