// Драйвер t02 для переведённых вариантов C- (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

int main() {
    const char* lines[] = {
        "width = 64",
        "  name=\"stone wall\"   # comment",
        "",
        "   # only comment",
        "enabled = true",
        "debug=false#x",
        "offset = -12",
        "big = 9223372036854775807",
        "too_big = 9223372036854775808",
        "min = -9223372036854775808",
        "Bad = 1",
        "9x = 1",
        "key 1",
        "key",
        "key = ",
        "key = truex",
        "key = \"open",
        "key = 12 13",
        "key-x = 1",
        "n = -92233720368547758080",
        "a_b2 = \"\"",
    };
    for (const char* line : lines) {
        cm::Result<std::optional<Entry>> r = parse_line(line);
        if (!r.ok()) { std::printf("error %s\n", r.err); continue; }
        if (!r.value) { std::printf("none\n"); continue; }
        const Entry& e = *r.value;
        switch (e.value.tag) {
            case Value::Tag::Int: std::printf("%s int %lld\n", e.key.c_str(), (long long)e.value.Int_._0); break;
            case Value::Tag::Bool: std::printf("%s bool %s\n", e.key.c_str(), e.value.Bool_._0 ? "true" : "false"); break;
            case Value::Tag::Str: std::printf("%s str [%s]\n", e.key.c_str(), e.value.Str_._0.c_str()); break;
        }
    }
    return 0;
}
