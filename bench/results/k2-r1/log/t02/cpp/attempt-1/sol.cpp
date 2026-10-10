// t02: заготовка — напишите тела функций по task.md.
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

namespace {

bool is_ws(char c) { return c == ' ' || c == '\t'; }

size_t skip_ws(std::string_view s, size_t p) {
    while (p < s.size() && is_ws(s[p])) ++p;
    return p;
}

bool is_key_start(char c) { return (c >= 'a' && c <= 'z') || c == '_'; }
bool is_key_char(char c) { return is_key_start(c) || (c >= '0' && c <= '9'); }

bool valid_key(std::string_view k) {
    if (k.empty() || !is_key_start(k[0])) return false;
    for (char c : k)
        if (!is_key_char(c)) return false;
    return true;
}

// Только пробелы и, возможно, комментарий до конца строки.
bool rest_is_blank(std::string_view s, size_t p) {
    p = skip_ws(s, p);
    return p >= s.size() || s[p] == '#';
}

bool parse_int(std::string_view t, int64_t& out) {
    size_t p = 0;
    bool neg = false;
    if (p < t.size() && t[p] == '-') {
        neg = true;
        ++p;
    }
    if (p >= t.size()) return false;
    const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX;
    uint64_t acc = 0;
    for (; p < t.size(); ++p) {
        char c = t[p];
        if (c < '0' || c > '9') return false;
        uint64_t d = (uint64_t)(c - '0');
        if (acc > (limit - d) / 10u) return false;
        acc = acc * 10u + d;
    }
    if (neg) {
        if (acc == (uint64_t)INT64_MAX + 1u)
            out = INT64_MIN;
        else
            out = -(int64_t)acc;
    } else {
        out = (int64_t)acc;
    }
    return true;
}

ParseResult fail(ParseError e) {
    ParseResult r;
    r.error = e;
    return r;
}

}  // namespace

ParseResult parse_line(std::string_view s) {
    size_t p = skip_ws(s, 0);
    if (p >= s.size() || s[p] == '#') return ParseResult{};

    // Ключ: всё до пробела, '=' или '#'.
    size_t ks = p;
    while (p < s.size() && !is_ws(s[p]) && s[p] != '=' && s[p] != '#') ++p;
    std::string_view key = s.substr(ks, p - ks);
    if (!valid_key(key)) return fail(ParseError::BadKey);

    p = skip_ws(s, p);
    if (p >= s.size() || s[p] != '=') return fail(ParseError::MissingEquals);
    ++p;

    p = skip_ws(s, p);
    if (p >= s.size() || s[p] == '#') return fail(ParseError::BadValue);

    Value v;
    v.kind = Value::Int;
    if (s[p] == '"') {
        size_t close = s.find('"', p + 1);
        if (close == std::string_view::npos) return fail(ParseError::BadValue);
        v.kind = Value::Str;
        v.s = std::string(s.substr(p + 1, close - p - 1));
        p = close + 1;
    } else {
        size_t ts = p;
        while (p < s.size() && !is_ws(s[p]) && s[p] != '#') ++p;
        std::string_view tok = s.substr(ts, p - ts);
        if (tok == "true") {
            v.kind = Value::Bool;
            v.b = true;
        } else if (tok == "false") {
            v.kind = Value::Bool;
            v.b = false;
        } else {
            int64_t n = 0;
            if (!parse_int(tok, n)) return fail(ParseError::BadValue);
            v.kind = Value::Int;
            v.i = n;
        }
    }
    if (!rest_is_blank(s, p)) return fail(ParseError::BadValue);

    ParseResult r;
    r.entry = Entry{std::string(key), std::move(v)};
    return r;
}
