// t16: эталон на C++20.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum class Tok { End, Num, Name, Op, Bad, Big };

struct Token {
    Tok kind;
    std::string text;
    int col;  // с 1
    int64_t value;
    int end;  // индекс байта после лексемы
};

enum class Kind { Num, Var, Neg, Bin, Assign };

struct Node {
    Kind kind;
    char op;
    int64_t value;
    std::string name;
    int left, right;
    int col;
};

struct Calc {
    std::string src;
    int pos = 0;
    bool has_peek = false;
    Token peeked{};
    std::vector<Node> nodes;
    bool failed = false;
    int err_col = 0;
    std::string err_msg;
    std::vector<std::pair<std::string, int64_t>> vars;
};

static bool is_name_start(char c) { return (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_name_char(char c) { return is_name_start(c) || (c >= '0' && c <= '9'); }

static Token lex(const std::string& s, int pos) {
    int n = (int)s.size();
    while (pos < n && s[pos] == ' ') pos++;
    if (pos >= n) return {Tok::End, "", n + 1, 0, n};
    char c = s[pos];
    int col = pos + 1;
    if (c >= '0' && c <= '9') {
        int end = pos;
        int64_t v = 0;
        bool big = false;
        while (end < n && s[end] >= '0' && s[end] <= '9') {
            int d = s[end] - '0';
            if (v > (INT64_MAX - d) / 10) big = true;
            else v = v * 10 + d;
            end++;
        }
        return {big ? Tok::Big : Tok::Num, s.substr(pos, end - pos), col, v, end};
    }
    if (is_name_start(c)) {
        int end = pos;
        while (end < n && is_name_char(s[end])) end++;
        return {Tok::Name, s.substr(pos, end - pos), col, 0, end};
    }
    if (std::string("+-*/%^()=;").find(c) != std::string::npos) return {Tok::Op, std::string(1, c), col, 0, pos + 1};
    return {Tok::Bad, std::string(1, c), col, 0, pos + 1};
}

static void fail(Calc& c, int col, const std::string& msg) {
    if (c.failed) return;
    c.failed = true;
    c.err_col = col;
    c.err_msg = msg;
}

static const Token& peek(Calc& c) {
    if (!c.has_peek) {
        c.peeked = lex(c.src, c.pos);
        c.has_peek = true;
        if (c.peeked.kind == Tok::Bad) fail(c, c.peeked.col, "unexpected character '" + c.peeked.text + "'");
        if (c.peeked.kind == Tok::Big) fail(c, c.peeked.col, "number too large");
    }
    return c.peeked;
}

static Token take(Calc& c) {
    Token t = peek(c);
    c.pos = t.end;
    c.has_peek = false;
    return t;
}

static bool is_op(Calc& c, char op) {
    const Token& t = peek(c);
    return !c.failed && t.kind == Tok::Op && t.text[0] == op;
}

static void unexpected(Calc& c) {
    const Token& t = peek(c);
    if (c.failed) return;
    if (t.kind == Tok::End) fail(c, t.col, "unexpected end");
    else fail(c, t.col, "unexpected '" + t.text + "'");
}

static int add(Calc& c, Node n) {
    c.nodes.push_back(n);
    return (int)c.nodes.size() - 1;
}

static int expr(Calc& c);
static int unary(Calc& c);

static int atom(Calc& c) {
    const Token& t = peek(c);
    if (c.failed) return -1;
    if (t.kind == Tok::Num) {
        Token n = take(c);
        return add(c, {Kind::Num, 0, n.value, "", -1, -1, n.col});
    }
    if (t.kind == Tok::Name) {
        Token n = take(c);
        return add(c, {Kind::Var, 0, 0, n.text, -1, -1, n.col});
    }
    if (is_op(c, '(')) {
        take(c);
        int e = expr(c);
        if (c.failed) return -1;
        if (!is_op(c, ')')) {
            const Token& u = peek(c);
            fail(c, u.col, "expected ')'");
            return -1;
        }
        take(c);
        return e;
    }
    unexpected(c);
    return -1;
}

static int power(Calc& c) {
    int a = atom(c);
    if (c.failed) return -1;
    if (is_op(c, '^')) {
        Token op = take(c);
        int b = unary(c);
        if (c.failed) return -1;
        return add(c, {Kind::Bin, '^', 0, "", a, b, op.col});
    }
    return a;
}

static int unary(Calc& c) {
    if (is_op(c, '-')) {
        Token op = take(c);
        int a = unary(c);
        if (c.failed) return -1;
        return add(c, {Kind::Neg, '-', 0, "", a, -1, op.col});
    }
    return power(c);
}

static int term(Calc& c) {
    int a = unary(c);
    while (!c.failed && (is_op(c, '*') || is_op(c, '/') || is_op(c, '%'))) {
        Token op = take(c);
        int b = unary(c);
        if (c.failed) return -1;
        a = add(c, {Kind::Bin, op.text[0], 0, "", a, b, op.col});
    }
    return a;
}

static int expr(Calc& c) {
    int a = term(c);
    while (!c.failed && (is_op(c, '+') || is_op(c, '-'))) {
        Token op = take(c);
        int b = term(c);
        if (c.failed) return -1;
        a = add(c, {Kind::Bin, op.text[0], 0, "", a, b, op.col});
    }
    return a;
}

static int stmt(Calc& c) {
    const Token& t = peek(c);
    if (c.failed) return -1;
    if (t.kind == Tok::Name) {
        Token after = lex(c.src, t.end);
        if (after.kind == Tok::Op && after.text == "=") {
            Token name = take(c);
            Token op = take(c);
            int e = expr(c);
            if (c.failed) return -1;
            return add(c, {Kind::Assign, '=', 0, name.text, e, -1, op.col});
        }
    }
    return expr(c);
}

static bool power_of(int64_t a, int64_t b, int64_t& out) {
    if (b == 0) { out = 1; return true; }
    if (a == 0 || a == 1) { out = a; return true; }
    if (a == -1) { out = b % 2 == 0 ? 1 : -1; return true; }
    if (b > 63) return false;
    int64_t r = 1;
    for (int64_t i = 0; i < b; i++)
        if (__builtin_mul_overflow(r, a, &r)) return false;
    out = r;
    return true;
}

static bool eval(Calc& c, int i, int64_t& out) {
    const Node n = c.nodes[i];
    switch (n.kind) {
        case Kind::Num: out = n.value; return true;
        case Kind::Var:
            for (const auto& v : c.vars)
                if (v.first == n.name) { out = v.second; return true; }
            fail(c, n.col, "unknown variable '" + n.name + "'");
            return false;
        case Kind::Neg: {
            int64_t a;
            if (!eval(c, n.left, a)) return false;
            if (a == INT64_MIN) { fail(c, n.col, "overflow"); return false; }
            out = -a;
            return true;
        }
        case Kind::Assign: {
            int64_t a;
            if (!eval(c, n.left, a)) return false;
            bool found = false;
            for (auto& v : c.vars)
                if (v.first == n.name) { v.second = a; found = true; }
            if (!found) c.vars.push_back({n.name, a});
            out = a;
            return true;
        }
        case Kind::Bin: {
            int64_t a, b;
            if (!eval(c, n.left, a) || !eval(c, n.right, b)) return false;
            bool over = false;
            switch (n.op) {
                case '+': over = __builtin_add_overflow(a, b, &out); break;
                case '-': over = __builtin_sub_overflow(a, b, &out); break;
                case '*': over = __builtin_mul_overflow(a, b, &out); break;
                case '/':
                case '%':
                    if (b == 0) { fail(c, n.col, "division by zero"); return false; }
                    if (b == -1) {
                        if (n.op == '%') { out = 0; return true; }
                        over = a == INT64_MIN;
                        out = over ? 0 : -a;
                    } else {
                        out = n.op == '/' ? a / b : a % b;
                    }
                    break;
                case '^':
                    if (b < 0) { fail(c, n.col, "negative exponent"); return false; }
                    over = !power_of(a, b, out);
                    break;
            }
            if (over) { fail(c, n.col, "overflow"); return false; }
            return true;
        }
    }
    return false;
}

std::string calc(const std::string& src) {
    Calc c;
    c.src = src;
    std::vector<int> stmts;
    stmts.push_back(stmt(c));
    while (!c.failed && is_op(c, ';')) {
        take(c);
        stmts.push_back(stmt(c));
    }
    if (!c.failed && peek(c).kind != Tok::End) unexpected(c);
    int64_t v = 0;
    for (size_t k = 0; k < stmts.size() && !c.failed; k++) eval(c, stmts[k], v);
    if (c.failed) return "error at " + std::to_string(c.err_col) + ": " + c.err_msg;
    return std::to_string(v);
}

// --- driver ---
static const char* CASES[] = {
    "1 + 2 * 3",
    "(1 + 2) * 3",
    "7 - 2 - 3",
    "100 / 7 / 2",
    "-7 / 2",
    "-7 % 2",
    "7 % -2",
    "2 ^ 3 ^ 2",
    "-2 ^ 2",
    "(-2) ^ 2",
    "2 ^ -1",
    "2 ^ - 1 + x",
    "0 ^ 0",
    "1 ^ 1000000000000",
    "-1 ^ 1000000000001",
    "(-1) ^ 1000000000001",
    "(-2) ^ 63",
    "2 ^ 63",
    "2 ^ 62 + (2 ^ 62 - 1)",
    "--3",
    "- - 3 - -3",
    "9223372036854775807",
    "9223372036854775808",
    "-9223372036854775808",
    "-9223372036854775807 - 1",
    "-9223372036854775807 - 2",
    "x = -9223372036854775807 - 1; x / -1",
    "x = -9223372036854775807 - 1; x % -1",
    "x = -9223372036854775807 - 1; -x",
    "x = -9223372036854775807 - 1; x * -1",
    "3037000500 * 3037000500",
    "3037000499 * 3037000499",
    "x = 5; y = x * 2; y + x",
    "x = 1; x = x + 1; x = x * 10; x",
    "a = b = 3",
    "x = 4",
    "_tmp = 3; _tmp2 = _tmp ^ 2; _tmp2",
    "x + 1",
    "1 / 0 + x",
    "x + 1 / 0",
    "y = 1 / 0; z",
    "5 % 0",
    "1 / (2 - 2)",
    "",
    "   ",
    "1 +",
    "1 + ",
    "(1 + 2",
    "(1 + 2 ",
    "(1 2)",
    "(1 + )",
    "()",
    "1 2",
    "2x",
    "x y",
    "1 + * 2",
    "1 ; ; 2",
    "1;",
    ";",
    "= 3",
    "x =",
    "1 + 2 $ 3",
    "1 2 $",
    "(1 $",
    "1 + Abc",
    "99999999999999999999 + 1",
    "1 2 99999999999999999999",
    "(4 + 5) * (2 - 7) ^ 2 % 1000 - -(3)",
    "a=2;b=a^a^a;b%1000003",
    "   42   ",
    "y + 1; y = 2",
    "x = 2; x ^ 62 * 2",
    "10 % 3 ^ 2",
};

int main() {
    for (const char* s : CASES) std::printf("[%s] => %s\n", s, calc(s).c_str());
    return 0;
}
