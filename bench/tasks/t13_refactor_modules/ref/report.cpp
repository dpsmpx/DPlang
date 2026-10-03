#include "report.hpp"

std::string describe(const World& w) {
    std::string out;
    for (size_t i = 0; i < w.units.size(); i++) {
        if (i) out += ' ';
        const Unit& u = w.units[i];
        out += std::to_string(i) + ":" + (u.hp > 0 ? std::to_string(u.hp) : std::string("dead"));
    }
    return out;
}

int32_t alive_count(const World& w) {
    int32_t n = 0;
    for (const Unit& u : w.units) n += u.hp > 0 ? 1 : 0;
    return n;
}
