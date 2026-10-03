// Драйвер t07 для переведённых вариантов DPlang (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

static void run(const std::vector<std::string>& rows) {
    auto r = shortest_path(rows);
    if (r) std::printf("path %d\n", *r); else std::printf("path none\n");
}

int main() {
    run({"S..G"});
    run({"S#.",
         ".#.",
         "..G"});
    run({"S.#.",
         ".##.",
         "...#",
         "#.#G"});
    run({"S.........",
         "########..",
         "..........",
         ".#########",
         "........G."});
    run({"SG"});
    run({"S#G"});
    return 0;
}
