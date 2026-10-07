// Строгий разбор JSON (json_read.h).
#include "support/json_read.h"

#include <set>

#include "support/utf8.h"

namespace dp::json {

namespace {

class Reader {
public:
    Reader(std::string_view text, uint32_t max_depth) : s_(text), max_depth_(max_depth) {}

    bool document(Value& out) {
        space();
        if (!value(out, 0)) return false;
        space();
        if (i_ != s_.size()) return fail("unexpected data after the JSON value");
        return true;
    }

    const std::string& error() const { return error_; }

private:
    std::string_view s_;
    size_t i_ = 0;
    uint32_t max_depth_;
    std::string error_;

    bool fail(std::string_view what) {
        if (error_.empty()) error_ = "invalid JSON at byte offset " + std::to_string(i_) + ": " + std::string(what);
        return false;
    }

    bool at_end() const { return i_ >= s_.size(); }
    char peek() const { return at_end() ? '\0' : s_[i_]; }

    // Пробелы RFC 8259: пробел, табуляция, перевод строки, возврат каретки.
    void space() {
        while (!at_end() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }

    bool word(std::string_view w) {
        if (s_.substr(i_, w.size()) != w) return fail("unexpected character");
        i_ += w.size();
        return true;
    }

    bool value(Value& out, uint32_t depth) {
        if (at_end()) return fail("unexpected end of input");
        switch (peek()) {
            case '{': return object(out, depth + 1);
            case '[': return array(out, depth + 1);
            case '"':
                out.kind = Kind::String;
                return string(out.text);
            case 't':
                out.kind = Kind::Bool;
                out.boolean = true;
                return word("true");
            case 'f':
                out.kind = Kind::Bool;
                out.boolean = false;
                return word("false");
            case 'n':
                out.kind = Kind::Null;
                return word("null");
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return number(out);
                return fail("unexpected character");
        }
    }

    bool object(Value& out, uint32_t depth) {
        if (depth > max_depth_) return fail("nesting is too deep");
        out.kind = Kind::Object;
        i_++;  // {
        space();
        if (peek() == '}') {
            i_++;
            return true;
        }
        std::set<std::string> seen;
        for (;;) {
            space();
            if (peek() != '"') return fail("expected a string key");
            std::string key;
            size_t key_at = i_;
            if (!string(key)) return false;
            if (!seen.insert(key).second) {
                i_ = key_at;
                return fail("duplicate key");
            }
            space();
            if (peek() != ':') return fail("expected ':'");
            i_++;
            space();
            out.keys.push_back(std::move(key));
            if (!value(out.items.emplace_back(), depth)) return false;
            space();
            if (peek() == ',') {
                i_++;
                continue;
            }
            if (peek() == '}') {
                i_++;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool array(Value& out, uint32_t depth) {
        if (depth > max_depth_) return fail("nesting is too deep");
        out.kind = Kind::Array;
        i_++;  // [
        space();
        if (peek() == ']') {
            i_++;
            return true;
        }
        for (;;) {
            space();
            if (!value(out.items.emplace_back(), depth)) return false;
            space();
            if (peek() == ',') {
                i_++;
                continue;
            }
            if (peek() == ']') {
                i_++;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool digits() {
        if (at_end() || peek() < '0' || peek() > '9') return fail("expected a digit");
        while (!at_end() && peek() >= '0' && peek() <= '9') i_++;
        return true;
    }

    // -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    bool number(Value& out) {
        size_t start = i_;
        if (peek() == '-') i_++;
        if (peek() == '0') {
            i_++;
            if (!at_end() && peek() >= '0' && peek() <= '9') return fail("leading zero in a number");
        } else if (!digits()) {
            return false;
        }
        if (peek() == '.') {
            i_++;
            if (!digits()) return false;
        }
        if (peek() == 'e' || peek() == 'E') {
            i_++;
            if (peek() == '+' || peek() == '-') i_++;
            if (!digits()) return false;
        }
        out.kind = Kind::Number;
        out.text = std::string(s_.substr(start, i_ - start));
        return true;
    }

    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return fail("truncated \\u escape");
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s_[i_ + k];
            uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return fail("invalid \\u escape");
            v = v * 16 + d;
        }
        i_ += 4;
        return true;
    }

    bool string(std::string& out) {
        i_++;  // "
        for (;;) {
            if (at_end()) return fail("unterminated string");
            unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"') {
                i_++;
                return true;
            }
            if (c < 0x20) return fail("control character in a string");
            if (c == '\\') {
                if (!escape(out)) return false;
                continue;
            }
            uint32_t cp;
            uint32_t n = decode_utf8(s_, i_, cp);
            if (n == 0) return fail("invalid UTF-8");
            out.append(s_.substr(i_, n));
            i_ += n;
        }
    }

    bool escape(std::string& out) {
        i_++;  // обратная косая
        if (at_end()) return fail("unterminated string");
        char c = s_[i_++];
        switch (c) {
            case '"': out += '"'; return true;
            case '\\': out += '\\'; return true;
            case '/': out += '/'; return true;
            case 'b': out += '\b'; return true;
            case 'f': out += '\f'; return true;
            case 'n': out += '\n'; return true;
            case 'r': out += '\r'; return true;
            case 't': out += '\t'; return true;
            case 'u': break;
            default: i_--; return fail("invalid escape");
        }
        uint32_t cp = 0;
        if (!hex4(cp)) return false;
        if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("unpaired surrogate in \\u escape");
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (s_.substr(i_, 2) != "\\u") return fail("unpaired surrogate in \\u escape");
            i_ += 2;
            uint32_t lo = 0;
            if (!hex4(lo)) return false;
            if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate in \\u escape");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        append_utf8(out, cp);
        return true;
    }
};

}  // namespace

const Value* Value::get(std::string_view key) const {
    if (kind != Kind::Object) return nullptr;
    for (size_t i = 0; i < keys.size(); i++) {
        if (keys[i] == key) return &items[i];
    }
    return nullptr;
}

bool parse(std::string_view text, uint32_t max_depth, Value& out, std::string& error) {
    Reader r(text, max_depth);
    out = Value{};
    if (r.document(out)) return true;
    error = r.error();
    return false;
}

}  // namespace dp::json
