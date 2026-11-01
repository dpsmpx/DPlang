// Образцы `match` и полнота разбора (docs/spec/core.md, раздел 6).
#include <set>

#include "sema/checker.h"

namespace dp::sema {

using namespace ast;

void Checker::pat(Pat* p, const Type* subject) {
    if (!p) return;
    bool err_subject = !subject || subject->kind == TK::Error;
    auto mismatch = [&](const std::string& what) {
        if (!err_subject) error(Code::E0417, p->span, what + " does not fit a value of type '" + tn(subject) + "'");
    };
    switch (p->kind) {
        case PatKind::Wild: case PatKind::Error: break;
        case PatKind::Bind: {
            auto* b = static_cast<BindPat*>(p);
            b->local = declare(b->name, b->span, subject, false, false, true);
            break;
        }
        case PatKind::Lit: {
            auto* l = static_cast<LitPat*>(p);
            Expr* lit = l->lit;
            if (!lit || lit->kind == ExprKind::Error) break;
            if (lit->kind == ExprKind::Float) {
                error(Code::E0417, p->span, "floating point values cannot be matched; compare them with '<' and '>'");
                break;
            }
            if (err_subject) break;
            if (auto* i = as<IntExpr>(lit)) {
                if (!is_integer(subject)) {
                    mismatch("an integer pattern");
                    break;
                }
                lit->type = int_lit(i, subject, l->negative, p->span);
                break;
            }
            const Type* t = infer(lit, subject);
            if (t != subject && !(is_integer(t) && is_integer(subject) && convertible(t, subject))) {
                mismatch("a '" + tn(t) + "' pattern");
            }
            break;
        }
        case PatKind::None:
            if (!err_subject && subject->kind != TK::Optional) mismatch("'none'");
            break;
        case PatKind::Wrap: {
            auto* w = static_cast<WrapPat*>(p);
            const Type* inner = err();
            if (w->which == WrapKind::Some) {
                if (!err_subject && subject->kind == TK::Optional) inner = subject->elem;
                else mismatch("'some(...)'");
            } else if (!err_subject && subject->kind == TK::Result) {
                inner = w->which == WrapKind::Ok ? subject->elem : (subject->err ? subject->err : err());
            } else {
                mismatch(w->which == WrapKind::Ok ? "'ok(...)'" : "'err(...)'");
            }
            pat(w->inner, inner);
            break;
        }
        case PatKind::Variant: {
            auto* v = static_cast<VariantPat*>(p);
            const EnumDef* en = !err_subject && subject->kind == TK::Enum ? subject->edef : nullptr;
            if (!v->path.empty()) {
                // `E.V` и `m.E.V`: перечисление по пути должно быть типом значения.
                const EnumDef* path_enum = nullptr;
                const Symbol* head = r_.lookup(v->path[0]);
                if (v->path.size() == 1 && head && head->kind == SymKind::Enum) {
                    path_enum = head->en;
                } else if (v->path.size() == 2 && head && head->kind == SymKind::Module) {
                    const Symbol* s = r_.member(*head->mod, v->path[1], v->span);
                    if (s && s->kind == SymKind::Enum) path_enum = s->en;
                    else if (!s) break;
                }
                if (!path_enum) {
                    std::string path;
                    for (const std::string& part : v->path) path += (path.empty() ? "" : ".") + part;
                    error(Code::E0307, v->span, "cannot find enum '" + path + "'");
                    break;
                }
                if (en && path_enum != en) {
                    mismatch("a pattern of enum '" + path_enum->name + "'");
                    break;
                }
                en = path_enum;
            } else if (!en) {
                mismatch("a variant pattern");
                for (Pat* a : v->args) pat(a, err());
                for (FieldPat& fp : v->fields) {
                    if (fp.pat) pat(fp.pat, err());
                    else fp.local = declare(fp.name, fp.name_span, err(), false, false, true);
                }
                break;
            }
            int idx = en->find(v->name);
            if (idx < 0) {
                error(Code::E0312, v->name_span, "enum '" + en->name + "' has no variant '" + v->name + "'");
                break;
            }
            v->res.kind = ResKind::Variant;
            v->res.en = en;
            v->res.index = static_cast<uint32_t>(idx);
            const VariantDef& vd = en->variants[static_cast<size_t>(idx)];
            if (v->form == VariantForm::Unit) break;  // `.V` подходит к варианту с любыми данными
            if (v->form != vd.form) {
                error(Code::E0414, p->span, "variant '" + vd.name + "' is written " +
                                                (vd.form == VariantForm::Unit    ? "without data"
                                                 : vd.form == VariantForm::Tuple ? "with positional data '(...)'"
                                                                                 : "with fields '{ ... }'"));
                break;
            }
            if (v->form == VariantForm::Tuple) {
                if (v->args.size() != vd.types.size()) {
                    error(Code::E0407, p->span, "variant '" + vd.name + "' has " + std::to_string(vd.types.size()) +
                                                    " value" + (vd.types.size() == 1 ? "" : "s") + ", the pattern has " +
                                                    std::to_string(v->args.size()));
                    break;
                }
                for (size_t i = 0; i < v->args.size(); i++) pat(v->args[i], vd.types[i]);
            } else {
                for (FieldPat& fp : v->fields) {
                    const Type* ft = nullptr;
                    for (const FieldDef& f : vd.fields) {
                        if (f.name == fp.name) ft = f.type;
                    }
                    if (!ft) {
                        error(Code::E0413, fp.name_span, "variant '" + vd.name + "' has no field '" + fp.name + "'");
                        ft = err();
                    }
                    if (fp.pat) pat(fp.pat, ft);
                    else fp.local = declare(fp.name, fp.name_span, ft, false, false, true);
                }
            }
            break;
        }
        case PatKind::Or:
            or_depth_++;
            for (Pat* a : static_cast<OrPat*>(p)->alts) pat(a, subject);
            or_depth_--;
            break;
    }
}

namespace {

// Полнота и лишние ветви — алгоритм полезности над матрицей образцов
// (stmt.match): строка — образцы одной ветви по столбцам, столбец —
// значение своего типа. Конечный тип (перечисление, `?T`, `E!T`, `bool`)
// перебирается по конструкторам; у остальных конструкторы — литералы, и
// закрывает их только `_`.
struct SPat {
    enum class K : uint8_t { Wild, Ctor, Lit, Or } k = K::Wild;
    int ctor = 0;               // Ctor: номер конструктора в типе
    std::string lit;            // Lit: ключ литерала
    std::vector<SPat> args;     // Ctor: данные; Or: варианты
};
using Row = std::vector<SPat>;
using Types = std::vector<const Type*>;

// Число конструкторов конечного типа; -1 — бесконечный (литералы, `_`).
int ctor_count(const Type* t) {
    if (!t) return -1;
    switch (t->kind) {
        case TK::Enum: return static_cast<int>(t->edef->variants.size());
        case TK::Optional: case TK::Result: case TK::Bool: return 2;
        default: return -1;
    }
}

// Типы данных конструктора c: у варианта — поля по порядку объявления.
Types ctor_args(const Type* t, int c) {
    Types out;
    if (t->kind == TK::Enum) {
        const VariantDef& v = t->edef->variants[static_cast<size_t>(c)];
        if (v.form == VariantForm::Tuple) out = v.types;
        else if (v.form == VariantForm::Record) {
            for (const FieldDef& f : v.fields) out.push_back(f.type);
        }
    } else if (t->kind == TK::Optional) {
        if (c == 1) out.push_back(t->elem);
    } else if (t->kind == TK::Result) {
        out.push_back(c == 0 ? t->elem : t->err);
    }
    return out;
}

SPat wild() { return SPat{}; }

// Образец, который не подходит ни к чему: не того типа (о нём уже
// сообщила проверка образца) — не закрывает значений и не делает лишними
// следующие ветви.
SPat never(uint64_t& n) {  // n — счётчик: у каждого свой ключ, друг друга они не закрывают
    SPat s;
    s.k = SPat::K::Lit;
    s.lit = "\x01" + std::to_string(n++);
    return s;
}

SPat simplify(const Pat* p, const Type* t, uint64_t& n) {
    SPat s;
    if (!p || p->kind == PatKind::Wild || p->kind == PatKind::Bind || p->kind == PatKind::Error) return s;
    if (!t || t->kind == TK::Error) return never(n);
    switch (p->kind) {
        case PatKind::Wild: case PatKind::Bind: case PatKind::Error: return s;
        case PatKind::Or:
            s.k = SPat::K::Or;
            for (const Pat* a : static_cast<const OrPat*>(p)->alts) s.args.push_back(simplify(a, t, n));
            return s;
        case PatKind::Lit: {
            auto* l = static_cast<const LitPat*>(p);
            if (auto* b = as<const BoolExpr>(l->lit)) {
                if (t->kind != TK::Bool) return never(n);
                s.k = SPat::K::Ctor;
                s.ctor = b->value ? 1 : 0;
                return s;
            }
            s.k = SPat::K::Lit;
            if (auto* i = as<const IntExpr>(l->lit)) s.lit = std::string(l->negative ? "-" : "") + std::to_string(i->value);
            else if (auto* x = as<const StrExpr>(l->lit)) s.lit = "\"" + x->value;
            else if (auto* y = as<const ByteExpr>(l->lit)) s.lit = "b" + std::to_string(y->value);
            else return never(n);
            return s;
        }
        case PatKind::None:
            if (t->kind != TK::Optional) return never(n);
            s.k = SPat::K::Ctor;
            s.ctor = 0;
            return s;
        case PatKind::Wrap: {
            auto* w = static_cast<const WrapPat*>(p);
            bool some = w->which == WrapKind::Some;
            if ((some && t->kind != TK::Optional) || (!some && t->kind != TK::Result)) return never(n);
            s.k = SPat::K::Ctor;
            s.ctor = some || w->which == WrapKind::Err ? 1 : 0;
            Types at = ctor_args(t, s.ctor);
            s.args.push_back(simplify(w->inner, at.empty() ? nullptr : at[0], n));
            return s;
        }
        case PatKind::Variant: {
            auto* v = static_cast<const VariantPat*>(p);
            if (v->res.kind != ResKind::Variant || t->kind != TK::Enum || v->res.en != t->edef) return never(n);
            s.k = SPat::K::Ctor;
            s.ctor = static_cast<int>(v->res.index);
            const VariantDef& vd = t->edef->variants[v->res.index];
            Types at = ctor_args(t, s.ctor);
            s.args.assign(at.size(), wild());
            if (v->form == VariantForm::Tuple && vd.form == VariantForm::Tuple && v->args.size() == at.size()) {
                for (size_t i = 0; i < at.size(); i++) s.args[i] = simplify(v->args[i], at[i], n);
            } else if (v->form == VariantForm::Record && vd.form == VariantForm::Record) {
                for (const FieldPat& fp : v->fields) {
                    for (size_t i = 0; i < vd.fields.size(); i++) {
                        if (vd.fields[i].name == fp.name && fp.pat) s.args[i] = simplify(fp.pat, at[i], n);
                    }
                }
            }
            return s;
        }
    }
    return s;
}

// Строки, у которых первый образец — конструктор head (Ctor или Lit) или
// `_`, с данными вместо первого образца.
std::vector<Row> specialize(const std::vector<Row>& m, const SPat& head, size_t arity) {
    std::vector<Row> out;
    for (const Row& r : m) {
        std::vector<Row> rs{r};
        for (size_t i = 0; i < rs.size(); i++) {  // «или» в начале — по строке на вариант
            const SPat& h = rs[i][0];
            if (h.k == SPat::K::Or) {
                for (const SPat& a : h.args) {
                    Row x = rs[i];
                    x[0] = a;
                    rs.push_back(std::move(x));
                }
                continue;
            }
            bool hit = h.k == SPat::K::Wild || (h.k == head.k && (h.k == SPat::K::Ctor ? h.ctor == head.ctor : h.lit == head.lit));
            if (!hit) continue;
            Row x;
            if (h.k == SPat::K::Wild) x.assign(arity, wild());
            else x = h.args;
            x.insert(x.end(), rs[i].begin() + 1, rs[i].end());
            out.push_back(std::move(x));
        }
    }
    return out;
}

// Строки с `_` в начале — без первого столбца.
std::vector<Row> rest_of_wild(const std::vector<Row>& m) {
    std::vector<Row> out;
    for (const Row& r : m) {
        std::vector<Row> rs{r};
        for (size_t i = 0; i < rs.size(); i++) {
            const SPat& h = rs[i][0];
            if (h.k == SPat::K::Or) {
                for (const SPat& a : h.args) {
                    Row x = rs[i];
                    x[0] = a;
                    rs.push_back(std::move(x));
                }
            } else if (h.k == SPat::K::Wild) {
                out.emplace_back(rs[i].begin() + 1, rs[i].end());
            }
        }
    }
    return out;
}

// Конструкторы и литералы в начале строк.
void heads(const SPat& h, std::set<int>& ctors, std::set<std::string>& lits) {
    if (h.k == SPat::K::Ctor) ctors.insert(h.ctor);
    else if (h.k == SPat::K::Lit) lits.insert(h.lit);
    else if (h.k == SPat::K::Or) {
        for (const SPat& a : h.args) heads(a, ctors, lits);
    }
}

Types concat(Types a, const Types& b, size_t skip) {
    a.insert(a.end(), b.begin() + static_cast<std::ptrdiff_t>(skip), b.end());
    return a;
}

// Есть ли значение, которое подходит к q и ни к одной строке m.
bool useful(const std::vector<Row>& m, const Row& q, const Types& ts) {
    if (q.empty()) return m.empty();
    const SPat& h = q[0];
    if (h.k == SPat::K::Or) {
        for (const SPat& a : h.args) {
            Row x = q;
            x[0] = a;
            if (useful(m, x, ts)) return true;
        }
        return false;
    }
    if (h.k != SPat::K::Wild) {
        Types at = h.k == SPat::K::Ctor ? ctor_args(ts[0], h.ctor) : Types{};
        Row x = h.args;
        x.insert(x.end(), q.begin() + 1, q.end());
        return useful(specialize(m, h, at.size()), x, concat(at, ts, 1));
    }
    std::set<int> ctors;
    std::set<std::string> lits;
    for (const Row& r : m) heads(r[0], ctors, lits);
    int n = ctor_count(ts[0]);
    if (n > 0 && static_cast<int>(ctors.size()) == n) {
        for (int c = 0; c < n; c++) {
            SPat k;
            k.k = SPat::K::Ctor;
            k.ctor = c;
            Types at = ctor_args(ts[0], c);
            Row x(at.size(), wild());
            x.insert(x.end(), q.begin() + 1, q.end());
            if (useful(specialize(m, k, at.size()), x, concat(at, ts, 1))) return true;
        }
        return false;
    }
    return useful(rest_of_wild(m), Row(q.begin() + 1, q.end()), Types(ts.begin() + 1, ts.end()));
}

// Пример значения, которого не закрывает ни одна строка m: строка
// образцов по столбцам ts; false — все значения закрыты.
bool witness(const std::vector<Row>& m, const Types& ts, Row& out) {
    if (ts.empty()) {
        out.clear();
        return m.empty();
    }
    std::set<int> ctors;
    std::set<std::string> lits;
    for (const Row& r : m) heads(r[0], ctors, lits);
    int n = ctor_count(ts[0]);
    if (n > 0 && static_cast<int>(ctors.size()) == n) {
        for (int c = 0; c < n; c++) {
            SPat k;
            k.k = SPat::K::Ctor;
            k.ctor = c;
            Types at = ctor_args(ts[0], c);
            Row w;
            if (!witness(specialize(m, k, at.size()), concat(at, ts, 1), w)) continue;
            k.args.assign(w.begin(), w.begin() + static_cast<std::ptrdiff_t>(at.size()));
            out.assign(1, k);
            out.insert(out.end(), w.begin() + static_cast<std::ptrdiff_t>(at.size()), w.end());
            return true;
        }
        return false;
    }
    Row w;
    if (!witness(rest_of_wild(m), Types(ts.begin() + 1, ts.end()), w)) return false;
    SPat h;  // `_`, а у конечного типа с частью конструкторов — недостающий
    if (n > 0 && !ctors.empty()) {
        for (int c = 0; c < n; c++) {
            if (ctors.count(c)) continue;
            h.k = SPat::K::Ctor;
            h.ctor = c;
            h.args.assign(ctor_args(ts[0], c).size(), wild());
            break;
        }
    }
    out.assign(1, h);
    out.insert(out.end(), w.begin(), w.end());
    return true;
}

bool all_wild(const std::vector<SPat>& ps) {
    for (const SPat& p : ps) {
        if (p.k != SPat::K::Wild) return false;
    }
    return true;
}

// Образец для сообщения: `.V`, `.V(_, x)`, `.V { f: p }`, `some(_)`.
std::string show(const SPat& p, const Type* t) {
    if (p.k != SPat::K::Ctor || !t) return "_";
    Types at = ctor_args(t, p.ctor);
    switch (t->kind) {
        case TK::Bool: return p.ctor ? "true" : "false";
        case TK::Optional: return p.ctor ? "some(" + show(p.args[0], at[0]) + ")" : "none";
        case TK::Result: return std::string(p.ctor ? "err(" : "ok(") + show(p.args[0], at[0]) + ")";
        case TK::Enum: {
            const VariantDef& v = t->edef->variants[static_cast<size_t>(p.ctor)];
            std::string s = "." + v.name;
            if (all_wild(p.args)) return s;
            if (v.form == VariantForm::Tuple) {
                s += "(";
                for (size_t i = 0; i < p.args.size(); i++) s += (i ? ", " : "") + show(p.args[i], at[i]);
                return s + ")";
            }
            s += " {";
            bool first = true;
            for (size_t i = 0; i < p.args.size(); i++) {
                if (p.args[i].k == SPat::K::Wild) continue;
                s += std::string(first ? " " : ", ") + v.fields[i].name + ": " + show(p.args[i], at[i]);
                first = false;
            }
            return s + " }";
        }
        default: return "_";
    }
}

bool irrefutable(const SPat& p) {
    if (p.k == SPat::K::Wild) return true;
    if (p.k == SPat::K::Or) {
        for (const SPat& a : p.args) {
            if (irrefutable(a)) return true;
        }
    }
    return false;
}

}  // namespace

void Checker::exhaustive(MatchStmt* m, const Type* subject) {
    if (!subject || subject->kind == TK::Error) return;
    Types ts{subject};
    std::vector<Row> rows;
    uint64_t nevers = 0;
    bool full = false;  // прежние ветви закрывают всё: дальше лишнее всё
    for (MatchArm& a : m->arms) {
        if (!a.pat || a.pat->kind == PatKind::Error) continue;
        Row r{simplify(a.pat, subject, nevers)};
        if (full || !useful(rows, r, ts)) {
            error(Code::E0416, a.pat->span, full ? "this arm is unreachable: earlier arms cover every value"
                                                 : "this arm is unreachable: earlier arms cover these values");
        }
        rows.push_back(std::move(r));
        full = full || irrefutable(rows.back()[0]) || !useful(rows, Row{wild()}, ts);
    }
    int n = ctor_count(subject);
    if (n < 0) {
        if (useful(rows, Row{wild()}, ts)) {
            error(Code::E0415, m->subject->span,
                  "'match' on '" + tn(subject) + "' needs an arm '_ => ...' for other values");
        }
        return;
    }
    // По примеру на каждый случай верхнего уровня, который не закрыт.
    std::string missing;
    for (int c = 0; c < n; c++) {
        SPat k;
        k.k = SPat::K::Ctor;
        k.ctor = c;
        Types at = ctor_args(subject, c);
        Row w;
        if (!witness(specialize(rows, k, at.size()), at, w)) continue;
        k.args = w;
        missing += (missing.empty() ? "" : ", ") + show(k, subject);
    }
    if (!missing.empty()) error(Code::E0415, m->subject->span, "'match' does not cover " + missing);
}

}  // namespace dp::sema
