// t16: калькулятор целых выражений с переменными.
// Разбор и вычисление без рекурсии: лексемы -> обратная польская запись
// (сортировочная станция) с проверкой грамматики по состоянию
// «ждём операнд / ждём оператор»; вычисление — на стеке значений.
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace t16impl {

enum class TK { Num, Name, Op, End, Err };

struct Token {
    TK kind;
    char op;           // для Op
    int64_t val;       // для Num
    size_t col;        // с 1
    std::string text;  // текст лексемы; для Err — сообщение
};

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const char c = s[i];
        if (c == ' ') {
            ++i;
            continue;
        }
        const size_t col = i + 1;
        if (c >= '0' && c <= '9') {
            size_t j = i;
            int64_t v = 0;
            bool big = false;
            while (j < n && s[j] >= '0' && s[j] <= '9') {
                const int64_t d = s[j] - '0';
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
                out.push_back(Token{TK::Err, 0, 0, col, "number too large"});
                return out;
            }
            out.push_back(Token{TK::Num, 0, v, col, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || c == '_') {
            size_t j = i + 1;
            while (j < n && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= '0' && s[j] <= '9') ||
                             s[j] == '_')) {
                ++j;
            }
            out.push_back(Token{TK::Name, 0, 0, col, s.substr(i, j - i)});
            i = j;
            continue;
        }
        switch (c) {
            case '+':
            case '-':
            case '*':
            case '/':
            case '%':
            case '^':
            case '(':
            case ')':
            case '=':
            case ';':
                out.push_back(Token{TK::Op, c, 0, col, std::string(1, c)});
                ++i;
                continue;
            default:
                break;
        }
        out.push_back(Token{TK::Err, 0, 0, col, std::string("unexpected character '") + c + "'"});
        return out;
    }
    out.push_back(Token{TK::End, 0, 0, n + 1, std::string()});
    return out;
}

enum class RK { Num, Var, Neg, Bin, Paren };

struct Item {
    RK kind;
    size_t ti;  // индекс лексемы
};

struct Stmt {
    bool assign;
    size_t name_ti;
    std::vector<Item> rpn;
};

int prec(const Item& it, const std::vector<Token>& toks) {
    if (it.kind == RK::Neg) return 3;
    switch (toks[it.ti].op) {
        case '+':
        case '-':
            return 1;
        case '*':
        case '/':
        case '%':
            return 2;
        case '^':
            return 4;
        default:
            return 0;
    }
}

bool is_binop(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^';
}

std::string err(size_t col, const std::string& msg) {
    return "error at " + std::to_string(col) + ": " + msg;
}

const char* ipow(int64_t a, int64_t e, int64_t& r) {
    if (e < 0) return "negative exponent";
    if (e == 0) {
        r = 1;
        return nullptr;
    }
    if (a == 0 || a == 1) {
        r = a;
        return nullptr;
    }
    if (a == -1) {
        r = (e % 2 == 0) ? 1 : -1;
        return nullptr;
    }
    if (e >= 64) return "overflow";
    int64_t acc = 1;
    for (int64_t k = 0; k < e; ++k) {
        if (__builtin_mul_overflow(acc, a, &acc)) return "overflow";
    }
    r = acc;
    return nullptr;
}

const char* apply(char op, int64_t a, int64_t b, int64_t& r) {
    switch (op) {
        case '+':
            return __builtin_add_overflow(a, b, &r) ? "overflow" : nullptr;
        case '-':
            return __builtin_sub_overflow(a, b, &r) ? "overflow" : nullptr;
        case '*':
            return __builtin_mul_overflow(a, b, &r) ? "overflow" : nullptr;
        case '/':
            if (b == 0) return "division by zero";
            if (a == INT64_MIN && b == -1) return "overflow";
            r = a / b;
            return nullptr;
        case '%':
            if (b == 0) return "division by zero";
            if (b == -1) {
                r = 0;
                return nullptr;
            }
            r = a % b;
            return nullptr;
        case '^':
            return ipow(a, b, r);
        default:
            r = 0;
            return nullptr;
    }
}

}  // namespace t16impl

std::string calc(const std::string& src) {
    using namespace t16impl;
    const std::vector<Token> toks = lex(src);
    std::vector<Stmt> stmts;

    // Разбор.
    size_t p = 0;
    while (true) {
        Stmt st{false, 0, {}};
        if (toks[p].kind == TK::Name && toks[p + 1].kind == TK::Op && toks[p + 1].op == '=') {
            st.assign = true;
            st.name_ti = p;
            p += 2;
        }
        std::vector<Item> ops;
        size_t depth = 0;
        bool operand = true;
        while (true) {
            const Token& t = toks[p];
            if (t.kind == TK::Err) return err(t.col, t.text);
            if (operand) {
                if (t.kind == TK::Op && t.op == '-') {
                    ops.push_back(Item{RK::Neg, p});
                    ++p;
                    continue;
                }
                if (t.kind == TK::Op && t.op == '(') {
                    ops.push_back(Item{RK::Paren, p});
                    ++depth;
                    ++p;
                    continue;
                }
                if (t.kind == TK::Num) {
                    st.rpn.push_back(Item{RK::Num, p});
                    ++p;
                    operand = false;
                    continue;
                }
                if (t.kind == TK::Name) {
                    st.rpn.push_back(Item{RK::Var, p});
                    ++p;
                    operand = false;
                    continue;
                }
                if (t.kind == TK::End) return err(t.col, "unexpected end");
                return err(t.col, "unexpected '" + t.text + "'");
            }
            // Ждём оператор.
            if (t.kind == TK::Op && is_binop(t.op)) {
                const Item cur{RK::Bin, p};
                const int pc = prec(cur, toks);
                const bool right = (t.op == '^');
                while (!ops.empty() && ops.back().kind != RK::Paren) {
                    const int pt = prec(ops.back(), toks);
                    if (pt > pc || (pt == pc && !right)) {
                        st.rpn.push_back(ops.back());
                        ops.pop_back();
                    } else {
                        break;
                    }
                }
                ops.push_back(cur);
                ++p;
                operand = true;
                continue;
            }
            if (depth > 0) {
                if (t.kind == TK::Op && t.op == ')') {
                    while (ops.back().kind != RK::Paren) {
                        st.rpn.push_back(ops.back());
                        ops.pop_back();
                    }
                    ops.pop_back();
                    --depth;
                    ++p;
                    continue;
                }
                return err(t.col, "expected ')'");
            }
            if (t.kind == TK::End || (t.kind == TK::Op && t.op == ';')) {
                while (!ops.empty()) {
                    st.rpn.push_back(ops.back());
                    ops.pop_back();
                }
                break;
            }
            return err(t.col, "unexpected '" + t.text + "'");
        }
        stmts.push_back(std::move(st));
        if (toks[p].kind == TK::End) break;
        ++p;  // ';'
    }

    // Вычисление.
    std::unordered_map<std::string, int64_t> vars;
    std::vector<int64_t> vals;
    int64_t last = 0;
    for (const Stmt& st : stmts) {
        vals.clear();
        for (const Item& it : st.rpn) {
            const Token& t = toks[it.ti];
            switch (it.kind) {
                case RK::Num:
                    vals.push_back(t.val);
                    break;
                case RK::Var: {
                    const auto f = vars.find(t.text);
                    if (f == vars.end()) return err(t.col, "unknown variable '" + t.text + "'");
                    vals.push_back(f->second);
                    break;
                }
                case RK::Neg: {
                    const int64_t v = vals.back();
                    if (v == INT64_MIN) return err(t.col, "overflow");
                    vals.back() = -v;
                    break;
                }
                case RK::Bin: {
                    const int64_t b = vals.back();
                    vals.pop_back();
                    const int64_t a = vals.back();
                    int64_t r = 0;
                    const char* e = apply(t.op, a, b, r);
                    if (e != nullptr) return err(t.col, e);
                    vals.back() = r;
                    break;
                }
                case RK::Paren:
                    break;
            }
        }
        last = vals.back();
        if (st.assign) vars[toks[st.name_ti].text] = last;
    }
    return std::to_string(last);
}
