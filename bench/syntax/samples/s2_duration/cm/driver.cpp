// Драйвер образца s2 для переведённых вариантов dpl.
#include "cm_out.hpp"

static void show(const char* text) {
    cm::Result<int64_t> r = parse_duration(text);
    if (r.ok()) std::printf("[%s] = %lld = %s\n", text, (long long)r.value, format_duration(r.value).c_str());
    else std::printf("[%s] error %s\n", text, r.err);
}

int main() {
    for (const char* t : {"1h30m15s", " 45s ", "2h", "90m", "", "12", "5x", "h5", "1h-2m"}) show(t);
    cm::Result<int64_t> sum = total_seconds(std::vector<std::string>{"1h", "30m", "15s"});
    std::printf("total %lld\n", (long long)sum.value);
    cm::Result<int64_t> bad = total_seconds(std::vector<std::string>{"1h", "oops", "15s"});
    std::printf("total error %s\n", bad.ok() ? "none" : bad.err);
    std::printf("format 0 = %s\n", format_duration(0).c_str());
    return 0;
}
