// t10: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <vector>

struct Edit { int32_t x, y, z, block; };

enum class DecodeError { None, Truncated, BadValue, TrailingBytes };

struct DecodeResult { DecodeError error; std::vector<Edit> edits; };

static void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

static uint64_t zigzag(int64_t n) {
    return (static_cast<uint64_t>(n) << 1) ^ static_cast<uint64_t>(n >> 63);
}

static int64_t unzigzag(uint64_t u) {
    return static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
}

std::vector<uint8_t> encode(const std::vector<Edit>& edits) {
    std::vector<uint8_t> out;
    put_varint(out, static_cast<uint64_t>(edits.size()));
    int64_t px = 0, py = 0, pz = 0;
    for (const Edit& e : edits) {
        put_varint(out, zigzag(static_cast<int64_t>(e.x) - px));
        put_varint(out, zigzag(static_cast<int64_t>(e.y) - py));
        put_varint(out, zigzag(static_cast<int64_t>(e.z) - pz));
        put_varint(out, static_cast<uint64_t>(static_cast<uint32_t>(e.block)));
        px = e.x;
        py = e.y;
        pz = e.z;
    }
    return out;
}

// Читает varint; при ошибке возвращает её код.
static DecodeError get_varint(const std::vector<uint8_t>& in, size_t& pos, uint64_t& out) {
    uint64_t v = 0;
    for (int i = 0; i < 10; ++i) {
        if (pos >= in.size()) return DecodeError::Truncated;
        uint8_t b = in[pos++];
        uint64_t part = b & 0x7f;
        if (i == 9 && part > 1) return DecodeError::BadValue;  // не влезает в 64 бита
        v |= part << (7 * i);
        if ((b & 0x80) == 0) {
            out = v;
            return DecodeError::None;
        }
    }
    return DecodeError::BadValue;  // длиннее 10 байт
}

static DecodeError get_coord(const std::vector<uint8_t>& in, size_t& pos, int32_t prev, int32_t& out) {
    uint64_t u = 0;
    DecodeError err = get_varint(in, pos, u);
    if (err != DecodeError::None) return err;
    int64_t d = unzigzag(u);
    // prev в пределах i32: при |d| > 2^33 результат заведомо вне i32, а сумма не переполняет i64.
    const int64_t lim = int64_t(1) << 33;
    if (d > lim || d < -lim) return DecodeError::BadValue;
    int64_t c = static_cast<int64_t>(prev) + d;
    if (c < INT32_MIN || c > INT32_MAX) return DecodeError::BadValue;
    out = static_cast<int32_t>(c);
    return DecodeError::None;
}

DecodeResult decode(const std::vector<uint8_t>& in) {
    size_t pos = 0;
    uint64_t count = 0;
    DecodeError err = get_varint(in, pos, count);
    if (err != DecodeError::None) return {err, {}};
    std::vector<Edit> edits;
    int32_t px = 0, py = 0, pz = 0;
    for (uint64_t k = 0; k < count; ++k) {
        Edit e{};
        if ((err = get_coord(in, pos, px, e.x)) != DecodeError::None) return {err, {}};
        if ((err = get_coord(in, pos, py, e.y)) != DecodeError::None) return {err, {}};
        if ((err = get_coord(in, pos, pz, e.z)) != DecodeError::None) return {err, {}};
        uint64_t b = 0;
        if ((err = get_varint(in, pos, b)) != DecodeError::None) return {err, {}};
        if (b > 65535) return {DecodeError::BadValue, {}};
        e.block = static_cast<int32_t>(b);
        edits.push_back(e);
        px = e.x;
        py = e.y;
        pz = e.z;
    }
    if (pos != in.size()) return {DecodeError::TrailingBytes, {}};
    return {DecodeError::None, edits};
}
