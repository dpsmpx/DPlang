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

ParseResult parse_line(std::string_view s) {
    // ...
}
