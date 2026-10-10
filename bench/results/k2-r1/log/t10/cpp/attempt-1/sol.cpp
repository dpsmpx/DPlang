// t10: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <utility>
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

namespace {

struct Reader {
    const std::vector<uint8_t>& in;
    size_t pos = 0;

    DecodeError varint(uint64_t& v) {
        v = 0;
        for (int i = 0;; ++i) {
            if (i == 10) return DecodeError::BadValue;
            if (pos >= in.size()) return DecodeError::Truncated;
            uint8_t b = in[pos++];
            v |= static_cast<uint64_t>(b & 0x7f) << (7 * i);
            if ((b & 0x80) == 0) return DecodeError::None;
        }
    }

    DecodeError coord(int64_t prev, int32_t& outv) {
        uint64_t u = 0;
        DecodeError err = varint(u);
        if (err != DecodeError::None) return err;
        int64_t d = unzigzag(u);
        int64_t r = 0;
        if (__builtin_add_overflow(prev, d, &r)) return DecodeError::BadValue;
        if (r < INT32_MIN || r > INT32_MAX) return DecodeError::BadValue;
        outv = static_cast<int32_t>(r);
        return DecodeError::None;
    }
};

}  // namespace

DecodeResult decode(const std::vector<uint8_t>& in) {
    DecodeResult res{DecodeError::None, {}};
    Reader rd{in, 0};
    uint64_t count = 0;
    DecodeError err = rd.varint(count);
    if (err != DecodeError::None) {
        res.error = err;
        return res;
    }
    std::vector<Edit> edits;
    edits.reserve(count < in.size() ? static_cast<size_t>(count) : in.size());
    int64_t px = 0, py = 0, pz = 0;
    for (uint64_t k = 0; k < count; ++k) {
        Edit e{0, 0, 0, 0};
        if ((err = rd.coord(px, e.x)) != DecodeError::None ||
            (err = rd.coord(py, e.y)) != DecodeError::None ||
            (err = rd.coord(pz, e.z)) != DecodeError::None) {
            res.error = err;
            return res;
        }
        uint64_t b = 0;
        if ((err = rd.varint(b)) != DecodeError::None) {
            res.error = err;
            return res;
        }
        if (b > 65535) {
            res.error = DecodeError::BadValue;
            return res;
        }
        e.block = static_cast<int32_t>(b);
        px = e.x;
        py = e.y;
        pz = e.z;
        edits.push_back(e);
    }
    if (rd.pos != in.size()) {
        res.error = DecodeError::TrailingBytes;
        return res;
    }
    res.edits = std::move(edits);
    return res;
}
