// t16: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

enum class TokKind { Num, Name, Op, End, LexErr };

struct Tok {
    TokKind kind;
    std::string text;  // текст лексемы или сообщение ошибки лексики
    int col;           // с 1
    int64_t val;
    char op;
};

std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> toks;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == ' ') {
            ++i;
            continue;
        }
        int col = static_cast<int>(i) + 1;
        if (c >= '0' && c <= '9') {
            size_t j = i;
            bool too_large = false;
            int64_t v = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') {
                int d = s[j] - '0';
                if (!too_large) {
                    if (v > (INT64_MAX - d) / 10) {
                        too_large = true;
                    } else {
                        v = v * 10 + d;
                    }
                }
                ++j;
            }
            if (too_large) {
                toks.push_back(Tok{TokKind::LexErr, "number too large", col, 0, 0});
                return toks;
            }
            toks.push_back(Tok{TokKind::Num, s.substr(i, j - i), col, v, 0});
            i = j;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || c == '_') {
            size_t j = i + 1;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= '0' && s[j] <= '9') || s[j] == '_')) ++j;
            toks.push_back(Tok{TokKind::Name, s.substr(i, j - i), col, 0, 0});
            i = j;
            continue;
        }
        switch (c) {
            case '+': case '-': case '*': case '/': case '%':
            case '^': case '(': case ')': case '=': case ';':
                toks.push_back(Tok{TokKind::Op, std::string(1, c), col, 0, c});
                ++i;
                continue;
            default:
                break;
        }
        toks.push_back(Tok{TokKind::LexErr, std::string("unexpected character '") + c + "'", col, 0, 0});
        return toks;
    }
    toks.push_back(Tok{TokKind::End, "", static_cast<int>(n) + 1, 0, 0});
    return toks;
}

enum class NodeKind { Num, Var, Neg, Bin };

struct Node {
    NodeKind kind;
    int col;
    int64_t val;
    std::string name;
    char op;
    int lhs;
    int rhs;
};

struct Stmt {
    bool assign;
    std::string name;
    int expr;
};

struct Parser {
    const std::vector<Tok>& toks;
    size_t pos = 0;
    std::vector<Node> nodes;
    bool failed = false;
    int err_col = 0;
    std::string err_msg;

    explicit Parser(const std::vector<Tok>& t) : toks(t) {}

    const Tok& peek() const { return toks[pos]; }
    const Tok& peek2() const { return toks[pos + 1 < toks.size() ? pos + 1 : pos]; }
    bool is_op(const Tok& t, char c) const { return t.kind == TokKind::Op && t.op == c; }

    void fail(int col, const std::string& msg) {
        if (failed) return;
        failed = true;
        err_col = col;
        err_msg = msg;
    }

    void fail_unexpected(const Tok& t) {
        if (t.kind == TokKind::LexErr) {
            fail(t.col, t.text);
        } else if (t.kind == TokKind::End) {
            fail(t.col, "unexpected end");
        } else {
            fail(t.col, "unexpected '" + t.text + "'");
        }
    }

    void fail_expected_paren(const Tok& t) {
        if (t.kind == TokKind::LexErr) {
            fail(t.col, t.text);
        } else {
            fail(t.col, "expected ')'");
        }
    }

    int add(const Node& n) {
        nodes.push_back(n);
        return static_cast<int>(nodes.size()) - 1;
    }

    int bin(char op, int col, int l, int r) {
        return add(Node{NodeKind::Bin, col, 0, "", op, l, r});
    }

    int atom() {
        const Tok& t = peek();
        if (t.kind == TokKind::Num) {
            ++pos;
            return add(Node{NodeKind::Num, t.col, t.val, "", 0, -1, -1});
        }
        if (t.kind == TokKind::Name) {
            ++pos;
            return add(Node{NodeKind::Var, t.col, 0, t.text, 0, -1, -1});
        }
        if (is_op(t, '(')) {
            ++pos;
            int e = expr();
            if (failed) return -1;
            if (is_op(peek(), ')')) {
                ++pos;
                return e;
            }
            fail_expected_paren(peek());
            return -1;
        }
        fail_unexpected(t);
        return -1;
    }

    int power() {
        int a = atom();
        if (failed) return -1;
        if (is_op(peek(), '^')) {
            int col = peek().col;
            ++pos;
            int b = unary();
            if (failed) return -1;
            return bin('^', col, a, b);
        }
        return a;
    }

    int unary() {
        if (is_op(peek(), '-')) {
            int col = peek().col;
            ++pos;
            int a = unary();
            if (failed) return -1;
            return add(Node{NodeKind::Neg, col, 0, "", '-', a, -1});
        }
        return power();
    }

    int term() {
        int a = unary();
        if (failed) return -1;
        while (is_op(peek(), '*') || is_op(peek(), '/') || is_op(peek(), '%')) {
            char op = peek().op;
            int col = peek().col;
            ++pos;
            int b = unary();
            if (failed) return -1;
            a = bin(op, col, a, b);
        }
        return a;
    }

    int expr() {
        int a = term();
        if (failed) return -1;
        while (is_op(peek(), '+') || is_op(peek(), '-')) {
            char op = peek().op;
            int col = peek().col;
            ++pos;
            int b = term();
            if (failed) return -1;
            a = bin(op, col, a, b);
        }
        return a;
    }

    bool stmt(std::vector<Stmt>& out) {
        if (peek().kind == TokKind::Name && is_op(peek2(), '=')) {
            std::string name = peek().text;
            pos += 2;
            int e = expr();
            if (failed) return false;
            out.push_back(Stmt{true, name, e});
            return true;
        }
        int e = expr();
        if (failed) return false;
        out.push_back(Stmt{false, "", e});
        return true;
    }

    bool program(std::vector<Stmt>& out) {
        if (!stmt(out)) return false;
        while (is_op(peek(), ';')) {
            ++pos;
            if (!stmt(out)) return false;
        }
        if (peek().kind != TokKind::End) {
            fail_unexpected(peek());
            return false;
        }
        return true;
    }
};

struct Evaluator {
    const std::vector<Node>& nodes;
    std::map<std::string, int64_t> vars;
    bool failed = false;
    int err_col = 0;
    std::string err_msg;

    explicit Evaluator(const std::vector<Node>& n) : nodes(n) {}

    void fail(int col, const std::string& msg) {
        if (failed) return;
        failed = true;
        err_col = col;
        err_msg = msg;
    }

    bool ipow(int64_t base, int64_t e, int64_t& out) {
        if (e == 0) { out = 1; return true; }
        if (base == 0) { out = 0; return true; }
        if (base == 1) { out = 1; return true; }
        if (base == -1) { out = (e % 2 == 0) ? 1 : -1; return true; }
        int64_t r = 1;
        for (int64_t k = 0; k < e; ++k) {
            int64_t nr = 0;
            if (__builtin_mul_overflow(r, base, &nr)) return false;
            r = nr;
        }
        out = r;
        return true;
    }

    int64_t eval(int idx) {
        const Node& n = nodes[idx];
        switch (n.kind) {
            case NodeKind::Num:
                return n.val;
            case NodeKind::Var: {
                auto it = vars.find(n.name);
                if (it == vars.end()) {
                    fail(n.col, "unknown variable '" + n.name + "'");
                    return 0;
                }
                return it->second;
            }
            case NodeKind::Neg: {
                int64_t a = eval(n.lhs);
                if (failed) return 0;
                if (a == INT64_MIN) {
                    fail(n.col, "overflow");
                    return 0;
                }
                return -a;
            }
            case NodeKind::Bin: {
                int64_t a = eval(n.lhs);
                if (failed) return 0;
                int64_t b = eval(n.rhs);
                if (failed) return 0;
                int64_t r = 0;
                switch (n.op) {
                    case '+':
                        if (__builtin_add_overflow(a, b, &r)) { fail(n.col, "overflow"); return 0; }
                        return r;
                    case '-':
                        if (__builtin_sub_overflow(a, b, &r)) { fail(n.col, "overflow"); return 0; }
                        return r;
                    case '*':
                        if (__builtin_mul_overflow(a, b, &r)) { fail(n.col, "overflow"); return 0; }
                        return r;
                    case '/':
                        if (b == 0) { fail(n.col, "division by zero"); return 0; }
                        if (a == INT64_MIN && b == -1) { fail(n.col, "overflow"); return 0; }
                        return a / b;
                    case '%':
                        if (b == 0) { fail(n.col, "division by zero"); return 0; }
                        if (b == -1) return 0;
                        return a % b;
                    case '^':
                        if (b < 0) { fail(n.col, "negative exponent"); return 0; }
                        if (!ipow(a, b, r)) { fail(n.col, "overflow"); return 0; }
                        return r;
                    default:
                        return 0;
                }
            }
        }
        return 0;
    }
};

std::string format_error(int col, const std::string& msg) {
    return "error at " + std::to_string(col) + ": " + msg;
}

}  // namespace

std::string calc(const std::string& src) {
    std::vector<Tok> toks = lex(src);
    Parser p(toks);
    std::vector<Stmt> stmts;
    if (!p.program(stmts)) {
        return format_error(p.err_col, p.err_msg);
    }
    Evaluator ev(p.nodes);
    int64_t last = 0;
    for (const Stmt& s : stmts) {
        int64_t v = ev.eval(s.expr);
        if (ev.failed) return format_error(ev.err_col, ev.err_msg);
        if (s.assign) ev.vars[s.name] = v;
        last = v;
    }
    return std::to_string(last);
}
