// t16: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

enum class TokKind { Num, Name, Op, End, Err };

struct Tok {
    TokKind kind;
    char op = 0;
    int64_t val = 0;
    std::string text;
    int col = 0;
    std::string errmsg;  // только для Err
};

// Лексемы до первой ошибки лексики; последняя — End или Err.
std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == ' ') {
            ++i;
            continue;
        }
        Tok t;
        t.col = static_cast<int>(i) + 1;
        if (c >= '0' && c <= '9') {
            size_t j = i;
            int64_t v = 0;
            bool too_large = false;
            while (j < n && s[j] >= '0' && s[j] <= '9') {
                if (!too_large) {
                    int64_t m = 0;
                    if (__builtin_mul_overflow(v, static_cast<int64_t>(10), &m) ||
                        __builtin_add_overflow(m, static_cast<int64_t>(s[j] - '0'), &v)) {
                        too_large = true;
                    }
                }
                ++j;
            }
            if (too_large) {
                t.kind = TokKind::Err;
                t.errmsg = "number too large";
                out.push_back(t);
                return out;
            }
            t.kind = TokKind::Num;
            t.val = v;
            t.text = s.substr(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || c == '_') {
            size_t j = i + 1;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '_')) ++j;
            t.kind = TokKind::Name;
            t.text = s.substr(i, j - i);
            out.push_back(t);
            i = j;
            continue;
        }
        switch (c) {
            case '+': case '-': case '*': case '/': case '%':
            case '^': case '(': case ')': case '=': case ';':
                t.kind = TokKind::Op;
                t.op = c;
                t.text = std::string(1, c);
                out.push_back(t);
                ++i;
                continue;
            default:
                break;
        }
        t.kind = TokKind::Err;
        t.errmsg = std::string("unexpected character '") + c + "'";
        out.push_back(t);
        return out;
    }
    Tok e;
    e.kind = TokKind::End;
    e.col = static_cast<int>(n) + 1;
    out.push_back(e);
    return out;
}

enum class InsKind { Push, Load, Neg, Bin, Store };

struct Ins {
    InsKind kind;
    char op = 0;
    int col = 0;
    int64_t val = 0;
    std::string name;
};

struct Parser {
    std::vector<Tok> toks;
    size_t pos = 0;
    std::vector<Ins> code;
    bool failed = false;
    int err_col = 0;
    std::string err_msg;

    const Tok& peek() const { return toks[pos]; }
    bool is_op(const Tok& t, char op) const { return t.kind == TokKind::Op && t.op == op; }

    void fail_at(int col, const std::string& msg) {
        failed = true;
        err_col = col;
        err_msg = msg;
    }

    void fail_tok(const Tok& t) {
        if (t.kind == TokKind::Err) {
            fail_at(t.col, t.errmsg);
        } else if (t.kind == TokKind::End) {
            fail_at(t.col, "unexpected end");
        } else {
            fail_at(t.col, "unexpected '" + t.text + "'");
        }
    }

    void emit_bin(char op, int col) {
        Ins in;
        in.kind = InsKind::Bin;
        in.op = op;
        in.col = col;
        code.push_back(in);
    }

    void program() {
        for (;;) {
            stmt();
            if (failed) return;
            const Tok& t = peek();
            if (is_op(t, ';')) {
                ++pos;
                continue;
            }
            if (t.kind == TokKind::End) return;
            fail_tok(t);
            return;
        }
    }

    void stmt() {
        const Tok& t = peek();
        if (t.kind == TokKind::Name && is_op(toks[pos + 1], '=')) {
            Ins st;
            st.kind = InsKind::Store;
            st.name = t.text;
            st.col = t.col;
            pos += 2;
            expr();
            if (failed) return;
            code.push_back(st);
            return;
        }
        expr();
    }

    void expr() {
        term();
        if (failed) return;
        for (;;) {
            const Tok& t = peek();
            if (!(is_op(t, '+') || is_op(t, '-'))) return;
            char op = t.op;
            int col = t.col;
            ++pos;
            term();
            if (failed) return;
            emit_bin(op, col);
        }
    }

    void term() {
        unary();
        if (failed) return;
        for (;;) {
            const Tok& t = peek();
            if (!(is_op(t, '*') || is_op(t, '/') || is_op(t, '%'))) return;
            char op = t.op;
            int col = t.col;
            ++pos;
            unary();
            if (failed) return;
            emit_bin(op, col);
        }
    }

    void unary() {
        std::vector<int> minus_cols;
        while (is_op(peek(), '-')) {
            minus_cols.push_back(peek().col);
            ++pos;
        }
        power();
        if (failed) return;
        for (size_t k = minus_cols.size(); k > 0; --k) {
            Ins in;
            in.kind = InsKind::Neg;
            in.col = minus_cols[k - 1];
            code.push_back(in);
        }
    }

    void power() {
        atom();
        if (failed) return;
        const Tok& t = peek();
        if (is_op(t, '^')) {
            int col = t.col;
            ++pos;
            unary();
            if (failed) return;
            emit_bin('^', col);
        }
    }

    void atom() {
        const Tok& t = peek();
        if (t.kind == TokKind::Num) {
            Ins in;
            in.kind = InsKind::Push;
            in.val = t.val;
            in.col = t.col;
            code.push_back(in);
            ++pos;
            return;
        }
        if (t.kind == TokKind::Name) {
            Ins in;
            in.kind = InsKind::Load;
            in.name = t.text;
            in.col = t.col;
            code.push_back(in);
            ++pos;
            return;
        }
        if (is_op(t, '(')) {
            ++pos;
            expr();
            if (failed) return;
            const Tok& c = peek();
            if (is_op(c, ')')) {
                ++pos;
                return;
            }
            if (c.kind == TokKind::Err) {
                fail_tok(c);
                return;
            }
            fail_at(c.col, "expected ')'");
            return;
        }
        fail_tok(t);
    }
};

bool ipow(int64_t b, int64_t e, int64_t& out) {
    if (e == 0) {
        out = 1;
        return true;
    }
    if (b == 0 || b == 1) {
        out = b;
        return true;
    }
    if (b == -1) {
        out = (e & 1) ? -1 : 1;
        return true;
    }
    // |b| >= 2: переполнение наступит не позже чем через 63 умножения.
    int64_t r = 1;
    for (int64_t i = 0; i < e; ++i) {
        if (__builtin_mul_overflow(r, b, &r)) return false;
    }
    out = r;
    return true;
}

std::string err_str(int col, const std::string& msg) {
    return "error at " + std::to_string(col) + ": " + msg;
}

}  // namespace

std::string calc(const std::string& src) {
    Parser p;
    p.toks = lex(src);
    p.program();
    if (p.failed) return err_str(p.err_col, p.err_msg);

    std::map<std::string, int64_t> vars;
    std::vector<int64_t> st;
    for (const Ins& in : p.code) {
        switch (in.kind) {
            case InsKind::Push:
                st.push_back(in.val);
                break;
            case InsKind::Load: {
                auto it = vars.find(in.name);
                if (it == vars.end()) return err_str(in.col, "unknown variable '" + in.name + "'");
                st.push_back(it->second);
                break;
            }
            case InsKind::Neg: {
                int64_t v = st.back();
                if (v == INT64_MIN) return err_str(in.col, "overflow");
                st.back() = -v;
                break;
            }
            case InsKind::Store:
                vars[in.name] = st.back();
                break;
            case InsKind::Bin: {
                int64_t r = st.back();
                st.pop_back();
                int64_t l = st.back();
                int64_t res = 0;
                switch (in.op) {
                    case '+':
                        if (__builtin_add_overflow(l, r, &res)) return err_str(in.col, "overflow");
                        break;
                    case '-':
                        if (__builtin_sub_overflow(l, r, &res)) return err_str(in.col, "overflow");
                        break;
                    case '*':
                        if (__builtin_mul_overflow(l, r, &res)) return err_str(in.col, "overflow");
                        break;
                    case '/':
                        if (r == 0) return err_str(in.col, "division by zero");
                        if (l == INT64_MIN && r == -1) return err_str(in.col, "overflow");
                        res = l / r;
                        break;
                    case '%':
                        if (r == 0) return err_str(in.col, "division by zero");
                        res = (r == -1) ? 0 : l % r;
                        break;
                    case '^':
                        if (r < 0) return err_str(in.col, "negative exponent");
                        if (!ipow(l, r, res)) return err_str(in.col, "overflow");
                        break;
                    default:
                        break;
                }
                st.back() = res;
                break;
            }
        }
    }
    if (st.empty()) return "0";
    return std::to_string(st.back());
}
