#include "syntax/lexer.h"

#include <cstdio>
#include <string>

#include "syntax/literal.h"

namespace dp {

namespace {

struct TokInfo {
    std::string_view name;
    std::string_view text;  // написание или описание
    bool fixed;             // text — написание
};

constexpr TokInfo kTokens[] = {
#define DP_TOKEN(name, desc) {#name, desc, false},
#define DP_KEYWORD(name, text) {#name, text, true},
#define DP_RESERVED(name, text) {#name, text, true},
#define DP_PUNCT(name, text) {#name, text, true},
#include "syntax/tokens.def"
};

struct Word {
    std::string_view text;
    TokKind kind;
    bool reserved;
};

constexpr Word kWords[] = {
#define DP_KEYWORD(name, text) {text, TokKind::name, false},
#define DP_RESERVED(name, text) {text, TokKind::name, true},
#include "syntax/tokens.def"
};

struct Punct {
    std::string_view text;
    TokKind kind;
};

constexpr Punct kPuncts[] = {
#define DP_PUNCT(name, text) {text, TokKind::name},
#include "syntax/tokens.def"
};

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

std::string char_repr(std::string_view text, size_t i, uint32_t len) {
    uint32_t cp = 0;
    if (len && decode_utf8(text, i, cp) == len) {
        if (cp >= 0x20 && cp != 0x7F) return "'" + std::string(text.substr(i, len)) + "'";
    } else {
        cp = static_cast<unsigned char>(text[i]);
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "U+%04X", cp);
    return buf;
}

class Lexer {
public:
    Lexer(const SourceFile& file, FileId id, Diagnostics& diags) : text_(file.text()), id_(id), diags_(diags) {}

    std::vector<Token> run() {
        while (i_ < text_.size()) step();
        out_.push_back({TokKind::Eof, static_cast<uint32_t>(i_), static_cast<uint32_t>(i_)});
        return std::move(out_);
    }

private:
    std::string_view text_;
    FileId id_;
    Diagnostics& diags_;
    size_t i_ = 0;
    std::vector<Token> out_;

    char at(size_t k) const { return k < text_.size() ? text_[k] : '\0'; }

    void emit(TokKind kind, size_t begin) {
        out_.push_back({kind, static_cast<uint32_t>(begin), static_cast<uint32_t>(i_)});
    }

    void error(Code code, size_t begin, size_t end, std::string msg) {
        diags_.error(code, Span{id_, static_cast<uint32_t>(begin), static_cast<uint32_t>(end)}, std::move(msg));
    }

    // Проверяет UTF-8 участка [begin, end): каждая неверная
    // последовательность — сообщение E0101.
    void check_utf8(size_t begin, size_t end) {
        for (size_t k = begin; k < end;) {
            uint32_t cp;
            uint32_t n = decode_utf8(text_, k, cp);
            if (n == 0 || k + n > end) {
                size_t bad = k + 1;
                while (bad < end && (static_cast<unsigned char>(text_[bad]) & 0xC0) == 0x80) bad++;
                error(Code::E0101, k, bad, "invalid UTF-8 sequence");
                k = bad;
            } else {
                k += n;
            }
        }
    }

    void step() {
        size_t begin = i_;
        char c = text_[i_];
        if (c == '\n') {
            i_++;
            emit(TokKind::Newline, begin);
        } else if (c == '\r') {
            if (at(i_ + 1) == '\n') {
                i_ += 2;
                emit(TokKind::Newline, begin);
            } else {
                i_++;
                error(Code::E0108, begin, i_, "carriage return not followed by a line feed");
                emit(TokKind::Error, begin);
            }
        } else if (c == ' ' || c == '\t') {
            while (at(i_) == ' ' || at(i_) == '\t') i_++;
            emit(TokKind::Whitespace, begin);
        } else if (c == '/' && at(i_ + 1) == '/') {
            while (i_ < text_.size() && text_[i_] != '\n' && !(text_[i_] == '\r' && at(i_ + 1) == '\n')) i_++;
            check_utf8(begin, i_);
            emit(TokKind::Comment, begin);
        } else if (is_ident_start(c)) {
            ident(begin);
        } else if (is_digit(c)) {
            number(begin);
        } else if (c == '"') {
            string(begin);
        } else if (c == '\'') {
            byte(begin);
        } else if (!punct(begin)) {
            unexpected(begin);
        }
    }

    void ident(size_t begin) {
        while (is_ident_char(at(i_))) i_++;
        std::string_view w = text_.substr(begin, i_ - begin);
        for (const Word& kw : kWords) {
            if (kw.text == w) {
                if (kw.reserved) {
                    error(Code::E0109, begin, i_, "'" + std::string(w) + "' is a reserved word");
                }
                emit(kw.kind, begin);
                return;
            }
        }
        emit(TokKind::Ident, begin);
    }

    void number(size_t begin) {
        bool is_float = false;
        if (text_[i_] == '0' && (at(i_ + 1) == 'x' || at(i_ + 1) == 'X' || at(i_ + 1) == 'b' || at(i_ + 1) == 'B' ||
                                 at(i_ + 1) == 'o' || at(i_ + 1) == 'O')) {
            i_ += 2;
            while (is_ident_char(at(i_))) i_++;
        } else {
            while (is_digit(at(i_)) || at(i_) == '_') i_++;
            // Дробная часть — только если за точкой цифра: `1..5` — диапазон, `x.0` не бывает.
            if (at(i_) == '.' && is_digit(at(i_ + 1))) {
                is_float = true;
                i_++;
                while (is_digit(at(i_)) || at(i_) == '_') i_++;
            }
            if (at(i_) == 'e' || at(i_) == 'E') {
                size_t k = i_ + 1;
                if (at(k) == '+' || at(k) == '-') k++;
                if (is_digit(at(k))) {
                    is_float = true;
                    i_ = k;
                    while (is_digit(at(i_)) || at(i_) == '_') i_++;
                }
            }
            // Буквы и цифры сразу за литералом — часть ошибочного литерала.
            while (is_ident_char(at(i_))) i_++;
        }
        std::string_view t = text_.substr(begin, i_ - begin);
        LitStatus st;
        if (is_float) {
            double v;
            st = parse_float_literal(t, v);
            if (st == LitStatus::OutOfRange) error(Code::E0110, begin, i_, "float literal is out of range for f64");
        } else {
            uint64_t v;
            st = parse_int_literal(t, v);
            if (st == LitStatus::OutOfRange) error(Code::E0106, begin, i_, "integer literal does not fit in 64 bits");
        }
        if (st == LitStatus::Malformed) error(Code::E0107, begin, i_, "malformed number literal");
        emit(st == LitStatus::Ok ? (is_float ? TokKind::Float : TokKind::Int) : TokKind::Error, begin);
    }

    // Строка до закрывающей кавычки на той же строке исходника.
    void string(size_t begin) {
        i_++;
        bool ok = true;
        while (true) {
            char c = at(i_);
            if (i_ >= text_.size() || c == '\n' || (c == '\r' && at(i_ + 1) == '\n')) {
                error(Code::E0103, begin, i_, "unterminated string literal");
                emit(TokKind::Error, begin);
                return;
            }
            if (c == '"') {
                i_++;
                break;
            }
            if (c == '\\') {
                Escape e = parse_escape(text_, i_);
                if (e.len == 0) {
                    size_t n = e.bad_len ? e.bad_len : 1;
                    // Не заходить за конец строки исходника.
                    size_t end = i_ + n;
                    for (size_t k = i_ + 1; k < end && k < text_.size(); k++) {
                        if (text_[k] == '\n' || text_[k] == '\r') {
                            end = k;
                            break;
                        }
                    }
                    error(Code::E0104, i_, end, "invalid escape sequence");
                    ok = false;
                    i_ = end;
                } else {
                    if (!e.is_unicode && e.value > 0x7F) {
                        error(Code::E0104, i_, i_ + e.len, "\\x escape above \\x7f in a string literal");
                        ok = false;
                    }
                    i_ += e.len;
                }
                continue;
            }
            unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x80) {
                if (u < 0x20 && c != '\t') {
                    error(Code::E0102, i_, i_ + 1, "control character " + char_repr(text_, i_, 1) +
                                                       " in string literal; write it as an escape");
                    ok = false;
                }
                i_++;
            } else {
                uint32_t cp;
                uint32_t n = decode_utf8(text_, i_, cp);
                if (n == 0) {
                    check_utf8(i_, i_ + 1);
                    ok = false;
                    n = 1;
                }
                i_ += n;
            }
        }
        emit(ok ? TokKind::Str : TokKind::Error, begin);
    }

    void byte(size_t begin) {
        i_++;
        bool ok = true;
        char c = at(i_);
        if (c == '\\') {
            Escape e = parse_escape(text_, i_);
            if (e.len == 0 || e.is_unicode) {
                ok = false;
                i_ += e.len ? e.len : (e.bad_len ? e.bad_len : 1);
            } else {
                i_ += e.len;
            }
        } else if (c >= 0x20 && c < 0x7F && c != '\'') {
            i_++;
        } else {
            ok = false;
            if (c != '\'' && c != '\n' && i_ < text_.size()) {
                uint32_t cp;
                uint32_t n = decode_utf8(text_, i_, cp);
                i_ += n ? n : 1;
            }
        }
        if (at(i_) == '\'') {
            i_++;
        } else {
            ok = false;
            // Ищем закрывающую кавычку в пределах строки, чтобы не плодить ошибки.
            size_t k = i_;
            while (k < text_.size() && text_[k] != '\n' && text_[k] != '\'' && k - i_ < 8) k++;
            if (at(k) == '\'') i_ = k + 1;
        }
        if (!ok) {
            check_utf8(begin, i_);
            error(Code::E0105, begin, i_, "invalid byte literal");
        }
        emit(ok ? TokKind::Byte : TokKind::Error, begin);
    }

    bool punct(size_t begin) {
        size_t best = 0;
        TokKind kind = TokKind::Error;
        for (const Punct& p : kPuncts) {
            if (p.text.size() > best && text_.substr(i_, p.text.size()) == p.text) {
                best = p.text.size();
                kind = p.kind;
            }
        }
        if (!best) return false;
        i_ += best;
        emit(kind, begin);
        return true;
    }

    // Подряд идущие недопустимые символы — один токен ошибки.
    void unexpected(size_t begin) {
        std::string first;
        bool bad_utf8 = false;
        while (i_ < text_.size()) {
            char c = text_[i_];
            if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '"' || c == '\'' || is_ident_char(c)) break;
            if (static_cast<unsigned char>(c) < 0x80) {
                bool is_punct = false;
                for (const Punct& p : kPuncts) {
                    if (p.text[0] == c) is_punct = true;
                }
                if (is_punct) break;
                if (first.empty()) first = char_repr(text_, i_, 1);
                i_++;
            } else {
                uint32_t cp;
                uint32_t n = decode_utf8(text_, i_, cp);
                if (n == 0) {
                    bad_utf8 = true;
                    n = 1;
                } else if (first.empty()) {
                    first = char_repr(text_, i_, n);
                }
                i_ += n;
            }
        }
        if (bad_utf8) check_utf8(begin, i_);
        if (!first.empty()) error(Code::E0102, begin, i_, "unexpected character " + first);
        emit(TokKind::Error, begin);
    }
};

}  // namespace

std::string_view tok_name(TokKind k) { return kTokens[static_cast<size_t>(k)].name; }

std::string tok_describe(TokKind k) {
    const TokInfo& t = kTokens[static_cast<size_t>(k)];
    return t.fixed ? "'" + std::string(t.text) + "'" : std::string(t.text);
}

bool is_trivia(TokKind k) { return k == TokKind::Whitespace || k == TokKind::Comment; }

bool is_keyword(TokKind k) {
    for (const Word& w : kWords) {
        if (w.kind == k) return true;
    }
    return false;
}

std::vector<Token> lex(const SourceFile& file, FileId id, Diagnostics& diags) {
    return Lexer(file, id, diags).run();
}

}  // namespace dp
