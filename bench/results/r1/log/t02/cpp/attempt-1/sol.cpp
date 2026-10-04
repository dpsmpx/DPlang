// t02: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

static bool is_ws(char c) { return c == ' ' || c == '\t'; }
static bool is_key_start(char c) { return (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_key_char(char c) { return is_key_start(c) || (c >= '0' && c <= '9'); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static size_t skip_ws(std::string_view s, size_t i) {
    while (i < s.size() && is_ws(s[i])) ++i;
    return i;
}

// После значения: только пробелы и, возможно, комментарий.
static bool rest_ok(std::string_view s, size_t i) {
    i = skip_ws(s, i);
    return i == s.size() || s[i] == '#';
}

static ParseResult fail(ParseError e) {
    ParseResult r;
    r.error = e;
    return r;
}

ParseResult parse_line(std::string_view s) {
    size_t n = s.size();
    size_t i = skip_ws(s, 0);
    if (i == n || s[i] == '#') return ParseResult{};

    // Ключ: лексема до пробела, '=', '#' или конца строки.
    size_t ks = i;
    while (i < n && !is_ws(s[i]) && s[i] != '=' && s[i] != '#') ++i;
    std::string_view key = s.substr(ks, i - ks);
    if (key.empty() || !is_key_start(key[0])) return fail(ParseError::BadKey);
    for (char c : key)
        if (!is_key_char(c)) return fail(ParseError::BadKey);

    i = skip_ws(s, i);
    if (i == n || s[i] != '=') return fail(ParseError::MissingEquals);
    ++i;
    i = skip_ws(s, i);
    if (i == n) return fail(ParseError::BadValue);

    Value v{};
    char c = s[i];
    if (c == '"') {
        size_t start = i + 1;
        size_t j = start;
        while (j < n && s[j] != '"') ++j;
        if (j == n) return fail(ParseError::BadValue);
        if (!rest_ok(s, j + 1)) return fail(ParseError::BadValue);
        v.kind = Value::Str;
        v.s = std::string(s.substr(start, j - start));
    } else if (c == '-' || is_digit(c)) {
        bool neg = false;
        size_t j = i;
        if (s[j] == '-') {
            neg = true;
            ++j;
        }
        if (j == n || !is_digit(s[j])) return fail(ParseError::BadValue);
        const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
        uint64_t acc = 0;
        while (j < n && is_digit(s[j])) {
            uint64_t d = (uint64_t)(s[j] - '0');
            if (acc > (limit - d) / 10u) return fail(ParseError::BadValue);
            acc = acc * 10u + d;
            ++j;
        }
        if (!rest_ok(s, j)) return fail(ParseError::BadValue);
        v.kind = Value::Int;
        if (neg) {
            v.i = (acc == (uint64_t)INT64_MAX + 1u) ? INT64_MIN : -(int64_t)acc;
        } else {
            v.i = (int64_t)acc;
        }
    } else {
        std::string_view rest = s.substr(i);
        if (rest.substr(0, 4) == "true" && rest_ok(s, i + 4)) {
            v.kind = Value::Bool;
            v.b = true;
        } else if (rest.substr(0, 5) == "false" && rest_ok(s, i + 5)) {
            v.kind = Value::Bool;
            v.b = false;
        } else {
            return fail(ParseError::BadValue);
        }
    }

    ParseResult r;
    Entry e;
    e.key = std::string(key);
    e.value = std::move(v);
    r.entry = std::move(e);
    return r;
}
