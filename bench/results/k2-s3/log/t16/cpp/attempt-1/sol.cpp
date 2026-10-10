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
    int col;
    std::string text;
    int64_t val;
};

// Лексемы до первой ошибки лексики; последняя — End или Err (текст Err — сообщение).
std::vector<Tok> lex(const std::string& src) {
    std::vector<Tok> toks;
    const size_t n = src.size();
    size_t i = 0;
    while (true) {
        while (i < n && src[i] == ' ') ++i;
        if (i >= n) {
            toks.push_back(Tok{TokKind::End, static_cast<int>(n) + 1, "", 0});
            break;
        }
        const char c = src[i];
        const int col = static_cast<int>(i) + 1;
        if (c >= '0' && c <= '9') {
            size_t j = i;
            int64_t v = 0;
            bool big = false;
            while (j < n && src[j] >= '0' && src[j] <= '9') {
                int64_t d = src[j] - '0';
                if (!big) {
                    if (v > (INT64_MAX - d) / 10) {
                        big = true;
                    } else {
                        v = v * 10 + d;
                    }
                }
                ++j;
            }
            if (big) {
                toks.push_back(Tok{TokKind::Err, col, "number too large", 0});
                break;
            }
            toks.push_back(Tok{TokKind::Num, col, src.substr(i, j - i), v});
            i = j;
        } else if ((c >= 'a' && c <= 'z') || c == '_') {
            size_t j = i;
            while (j < n && ((src[j] >= 'a' && src[j] <= 'z') || (src[j] >= '0' && src[j] <= '9') ||
                             src[j] == '_'))
                ++j;
            toks.push_back(Tok{TokKind::Name, col, src.substr(i, j - i), 0});
            i = j;
        } else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^' ||
                   c == '(' || c == ')' || c == '=' || c == ';') {
            toks.push_back(Tok{TokKind::Op, col, std::string(1, c), 0});
            ++i;
        } else {
            std::string msg = "unexpected character '";
            msg += c;
            msg += "'";
            toks.push_back(Tok{TokKind::Err, col, msg, 0});
            break;
        }
    }
    return toks;
}

enum class NodeKind { Num, Var, Neg, Bin, Assign };

struct Node {
    NodeKind kind;
    int col;
    char op;
    int64_t val;
    std::string name;
    int left;
    int right;
};

struct Parser {
    const std::vector<Tok>& toks;
    size_t pos = 0;
    bool failed = false;
    int ecol = 0;
    std::string emsg;
    std::vector<Node> nodes;
    std::vector<int> stmts;

    explicit Parser(const std::vector<Tok>& t) : toks(t) {}

    void fail(int col, const std::string& msg) {
        if (failed) return;
        failed = true;
        ecol = col;
        emsg = msg;
    }

    bool at_err() {
        if (toks[pos].kind == TokKind::Err) {
            fail(toks[pos].col, toks[pos].text);
            return true;
        }
        return false;
    }

    bool is_op(char c) const {
        const Tok& t = toks[pos];
        return t.kind == TokKind::Op && t.text[0] == c;
    }

    int add(NodeKind k, int col, char op, int64_t val, const std::string& name, int l, int r) {
        nodes.push_back(Node{k, col, op, val, name, l, r});
        return static_cast<int>(nodes.size()) - 1;
    }

    void unexpected() {
        const Tok& t = toks[pos];
        if (t.kind == TokKind::End) {
            fail(t.col, "unexpected end");
        } else {
            fail(t.col, "unexpected '" + t.text + "'");
        }
    }

    void program() {
        int s = stmt();
        if (failed) return;
        stmts.push_back(s);
        while (true) {
            if (at_err()) return;
            const Tok& t = toks[pos];
            if (t.kind == TokKind::End) return;
            if (is_op(';')) {
                ++pos;
                s = stmt();
                if (failed) return;
                stmts.push_back(s);
                continue;
            }
            unexpected();
            return;
        }
    }

    int stmt() {
        if (at_err()) return -1;
        const Tok& t = toks[pos];
        if (t.kind == TokKind::Name) {
            const Tok& nx = toks[pos + 1];
            if (nx.kind == TokKind::Op && nx.text[0] == '=') {
                pos += 2;
                int e = expr();
                if (failed) return -1;
                return add(NodeKind::Assign, t.col, '=', 0, t.text, e, -1);
            }
        }
        return expr();
    }

    int expr() {
        int l = term();
        if (failed) return -1;
        while (true) {
            if (at_err()) return -1;
            if (is_op('+') || is_op('-')) {
                const Tok& t = toks[pos];
                ++pos;
                int r = term();
                if (failed) return -1;
                l = add(NodeKind::Bin, t.col, t.text[0], 0, "", l, r);
            } else {
                return l;
            }
        }
    }

    int term() {
        int l = unary();
        if (failed) return -1;
        while (true) {
            if (at_err()) return -1;
            if (is_op('*') || is_op('/') || is_op('%')) {
                const Tok& t = toks[pos];
                ++pos;
                int r = unary();
                if (failed) return -1;
                l = add(NodeKind::Bin, t.col, t.text[0], 0, "", l, r);
            } else {
                return l;
            }
        }
    }

    int unary() {
        if (at_err()) return -1;
        if (is_op('-')) {
            const Tok& t = toks[pos];
            ++pos;
            int u = unary();
            if (failed) return -1;
            return add(NodeKind::Neg, t.col, '-', 0, "", u, -1);
        }
        return power();
    }

    int power() {
        int a = atom();
        if (failed) return -1;
        if (at_err()) return -1;
        if (is_op('^')) {
            const Tok& t = toks[pos];
            ++pos;
            int e = unary();
            if (failed) return -1;
            return add(NodeKind::Bin, t.col, '^', 0, "", a, e);
        }
        return a;
    }

    int atom() {
        if (at_err()) return -1;
        const Tok& t = toks[pos];
        if (t.kind == TokKind::Num) {
            ++pos;
            return add(NodeKind::Num, t.col, 0, t.val, "", -1, -1);
        }
        if (t.kind == TokKind::Name) {
            ++pos;
            return add(NodeKind::Var, t.col, 0, 0, t.text, -1, -1);
        }
        if (is_op('(')) {
            ++pos;
            int e = expr();
            if (failed) return -1;
            if (at_err()) return -1;
            if (is_op(')')) {
                ++pos;
                return e;
            }
            fail(toks[pos].col, "expected ')'");
            return -1;
        }
        unexpected();
        return -1;
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
        out = (e % 2 == 0) ? 1 : -1;
        return true;
    }
    if (e >= 64) return false;
    int64_t r = 1;
    for (int64_t i = 0; i < e; ++i) {
        if (__builtin_mul_overflow(r, b, &r)) return false;
    }
    out = r;
    return true;
}

struct Evaluator {
    const std::vector<Node>& nodes;
    std::map<std::string, int64_t> vars;
    bool failed = false;
    int ecol = 0;
    std::string emsg;

    explicit Evaluator(const std::vector<Node>& n) : nodes(n) {}

    bool fail(int col, const std::string& msg) {
        failed = true;
        ecol = col;
        emsg = msg;
        return false;
    }

    bool eval(int id, int64_t& out) {
        const Node& nd = nodes[id];
        switch (nd.kind) {
            case NodeKind::Num:
                out = nd.val;
                return true;
            case NodeKind::Var: {
                auto it = vars.find(nd.name);
                if (it == vars.end()) return fail(nd.col, "unknown variable '" + nd.name + "'");
                out = it->second;
                return true;
            }
            case NodeKind::Neg: {
                int64_t v = 0;
                if (!eval(nd.left, v)) return false;
                if (v == INT64_MIN) return fail(nd.col, "overflow");
                out = -v;
                return true;
            }
            case NodeKind::Assign: {
                int64_t v = 0;
                if (!eval(nd.left, v)) return false;
                vars[nd.name] = v;
                out = v;
                return true;
            }
            case NodeKind::Bin: {
                int64_t a = 0, b = 0;
                if (!eval(nd.left, a)) return false;
                if (!eval(nd.right, b)) return false;
                int64_t r = 0;
                switch (nd.op) {
                    case '+':
                        if (__builtin_add_overflow(a, b, &r)) return fail(nd.col, "overflow");
                        break;
                    case '-':
                        if (__builtin_sub_overflow(a, b, &r)) return fail(nd.col, "overflow");
                        break;
                    case '*':
                        if (__builtin_mul_overflow(a, b, &r)) return fail(nd.col, "overflow");
                        break;
                    case '/':
                        if (b == 0) return fail(nd.col, "division by zero");
                        if (a == INT64_MIN && b == -1) return fail(nd.col, "overflow");
                        r = a / b;
                        break;
                    case '%':
                        if (b == 0) return fail(nd.col, "division by zero");
                        if (b == -1) {
                            r = 0;
                        } else {
                            r = a % b;
                        }
                        break;
                    case '^':
                        if (b < 0) return fail(nd.col, "negative exponent");
                        if (!ipow(a, b, r)) return fail(nd.col, "overflow");
                        break;
                    default:
                        return fail(nd.col, "overflow");
                }
                out = r;
                return true;
            }
        }
        return false;
    }
};

std::string err(int col, const std::string& msg) {
    return "error at " + std::to_string(col) + ": " + msg;
}

}  // namespace

std::string calc(const std::string& src) {
    std::vector<Tok> toks = lex(src);
    Parser p(toks);
    p.program();
    if (p.failed) return err(p.ecol, p.emsg);
    Evaluator ev(p.nodes);
    int64_t last = 0;
    for (int s : p.stmts) {
        if (!ev.eval(s, last)) return err(ev.ecol, ev.emsg);
    }
    return std::to_string(last);
}
