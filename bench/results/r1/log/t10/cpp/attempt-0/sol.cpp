// t10: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <vector>

struct Edit { int32_t x, y, z, block; };

enum class DecodeError { None, Truncated, BadValue, TrailingBytes };

struct DecodeResult { DecodeError error; std::vector<Edit> edits; };

std::vector<uint8_t> encode(const std::vector<Edit>& edits) {
    // ...
}

DecodeResult decode(const std::vector<uint8_t>& in) {
    // ...
}
