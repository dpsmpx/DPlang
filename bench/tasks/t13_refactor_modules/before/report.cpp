#include "report.hpp"

std::string describe(const World& w) {
    std::string out;
    for (int i = 0; i < (int)w.units.size(); i++) {
        if (i) out += ' ';
        const Unit& u = w.units[i];
        out += std::to_string(i) + ":" + (u.alive ? std::to_string(u.hp) : std::string("dead"));
    }
    return out;
}

int alive_count(const World& w) {
    int n = 0;
    for (const Unit& u : w.units) n += u.alive ? 1 : 0;
    return n;
}
