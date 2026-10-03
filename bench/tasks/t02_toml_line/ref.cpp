// Эталон t02 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

enum class ParseError { None, BadKey, MissingEquals, BadValue };

struct Value {
    enum Kind { Int, Bool, Str } kind;
    int64_t i = 0;
    bool b = false;
    std::string s;
};

struct Entry {
    std::string key;
    Value value;
};

struct ParseResult {
    ParseError error = ParseError::None;
    std::optional<Entry> entry;
};

static bool is_space(char c) { return c == ' ' || c == '\t'; }
static bool key_start(char c) { return (c >= 'a' && c <= 'z') || c == '_'; }
static bool key_char(char c) { return key_start(c) || (c >= '0' && c <= '9'); }

// Остаток строки после значения: только пробелы и комментарий.
static bool rest_ok(std::string_view s, size_t i) {
    while (i < s.size() && is_space(s[i])) i++;
    return i == s.size() || s[i] == '#';
}

ParseResult parse_line(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) i++;
    if (i == s.size() || s[i] == '#') return {};
    if (!key_start(s[i])) return {ParseError::BadKey, {}};
    size_t k0 = i;
    while (i < s.size() && key_char(s[i])) i++;
    std::string key(s.substr(k0, i - k0));
    // Символ, недопустимый в ключе, сразу за ключом — ошибка ключа.
    if (i < s.size() && s[i] != '=' && !is_space(s[i]) && s[i] != '#')
        return {ParseError::BadKey, {}};
    while (i < s.size() && is_space(s[i])) i++;
    if (i == s.size() || s[i] != '=') return {ParseError::MissingEquals, {}};
    i++;
    while (i < s.size() && is_space(s[i])) i++;
    if (i == s.size()) return {ParseError::BadValue, {}};

    Value v{};
    if (s[i] == '"') {
        size_t j = i + 1;
        while (j < s.size() && s[j] != '"') j++;
        if (j == s.size()) return {ParseError::BadValue, {}};
        v.kind = Value::Str;
        v.s = std::string(s.substr(i + 1, j - i - 1));
        i = j + 1;
    } else if (s.substr(i, 4) == "true" && (i + 4 == s.size() || !key_char(s[i + 4]))) {
        v.kind = Value::Bool; v.b = true; i += 4;
    } else if (s.substr(i, 5) == "false" && (i + 5 == s.size() || !key_char(s[i + 5]))) {
        v.kind = Value::Bool; v.b = false; i += 5;
    } else {
        bool neg = false;
        if (s[i] == '-') { neg = true; i++; }
        if (i == s.size() || s[i] < '0' || s[i] > '9') return {ParseError::BadValue, {}};
        const uint64_t limit = (uint64_t)INT64_MAX + (neg ? 1 : 0);
        uint64_t mag = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            uint64_t d = (uint64_t)(s[i] - '0');
            if (mag > (limit - d) / 10) return {ParseError::BadValue, {}};
            mag = mag * 10 + d;
            i++;
        }
        v.kind = Value::Int;
        v.i = neg ? (int64_t)(0 - mag) : (int64_t)mag;
    }
    if (!rest_ok(s, i)) return {ParseError::BadValue, {}};
    return {ParseError::None, Entry{key, v}};
}

// --- driver ---
static const char* err_name(ParseError e) {
    switch (e) {
        case ParseError::BadKey: return "BadKey";
        case ParseError::MissingEquals: return "MissingEquals";
        case ParseError::BadValue: return "BadValue";
        default: return "None";
    }
}

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
        ParseResult r = parse_line(line);
        if (r.error != ParseError::None) { std::printf("error %s\n", err_name(r.error)); continue; }
        if (!r.entry) { std::printf("none\n"); continue; }
        const Entry& e = *r.entry;
        switch (e.value.kind) {
            case Value::Int: std::printf("%s int %lld\n", e.key.c_str(), (long long)e.value.i); break;
            case Value::Bool: std::printf("%s bool %s\n", e.key.c_str(), e.value.b ? "true" : "false"); break;
            case Value::Str: std::printf("%s str [%s]\n", e.key.c_str(), e.value.s.c_str()); break;
        }
    }
    return 0;
}
