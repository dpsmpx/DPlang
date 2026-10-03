// Драйвер образца s3 для переведённых вариантов DPlang.
#include "cm_out.hpp"

static void check(const char* what, cm::Result<void> r) {
    std::printf("%s: %s\n", what, r.ok() ? "ok" : r.err);
}

int main() {
    Bank b = bank_new();
    check("open 1", open(b, 1, 100));
    check("open 2", open(b, 2, 0));
    check("open 3", open(b, 3, -5));
    check("move 1->2 30", transfer(b, 1, 2, 30));
    check("move 2->1 50", transfer(b, 2, 1, 50));
    check("move 1->9 1", transfer(b, 1, 9, 1));
    check("move 1->2 0", transfer(b, 1, 2, 0));
    int32_t rejected = apply_all(b, std::vector<Op>{{1, 2, 10}, {2, 1, 100}, {2, 1, 40}, {5, 1, 1}});
    std::printf("rejected %d, total %lld, checksum %016llx\n", rejected, (long long)total(b),
                (unsigned long long)checksum(b));
    for (const Account& a : b.accounts) std::printf("account %d: %lld\n", a.id, (long long)a.balance);
    for (const std::string& line : b.log) std::printf("log %s\n", line.c_str());
    return 0;
}
