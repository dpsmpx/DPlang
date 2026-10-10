// Драйвер t10 для переведённых вариантов dpl (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void hex(const std::vector<uint8_t>& b) {
    for (size_t i = 0; i < b.size(); i++) std::printf(i ? " %02x" : "%02x", b[i]);
    std::printf("\n");
}

static void dec(const std::vector<uint8_t>& b) {
    cm::Result<std::vector<Edit>> r = decode(b);
    if (!r.ok()) { std::printf("decode error %s\n", r.err); return; }
    std::printf("decode %d:", (int)r.value.size());
    for (const Edit& e : r.value) std::printf(" (%d,%d,%d,%d)", e.x, e.y, e.z, e.block);
    std::printf("\n");
}

int main() {
    std::vector<Edit> edits = {{0, 0, 0, 1}, {1, 0, 0, 1}, {1, 64, -3, 300},
                               {-100000, 64, -3, 65535}, {2147483647, -2147483647 - 1, 0, 0}};
    std::vector<uint8_t> bytes = encode(edits);
    hex(bytes);
    dec(bytes);
    hex(encode(std::vector<Edit>{}));
    dec(encode(std::vector<Edit>{}));
    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 1);
    dec(cut);
    std::vector<uint8_t> extra = bytes; extra.push_back(0);
    dec(extra);
    dec({1, 0, 0, 0, 0x80, 0x80, 0x04});        // block = 65536
    dec({1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01});
    dec({});
    return 0;
}
