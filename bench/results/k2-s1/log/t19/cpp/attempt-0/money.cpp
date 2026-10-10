#include "money.hpp"

std::string fmt_money(int64_t cents) {
    std::string sign = cents < 0 ? "-" : "";
    int64_t a = cents < 0 ? -cents : cents;
    std::string frac = std::to_string(a % 100);
    if (frac.size() < 2) frac = "0" + frac;
    return sign + std::to_string(a / 100) + "." + frac;
}

int64_t parse_money(const std::string& s) {
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot > 12 || s.size() != dot + 3) return -1;
    int64_t v = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (i == dot) continue;
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}
