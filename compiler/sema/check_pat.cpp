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

// Образец подходит к любому значению.
bool irrefutable(const Pat* p) {
    if (!p) return true;
    switch (p->kind) {
        case PatKind::Wild: case PatKind::Bind: case PatKind::Error: return true;
        case PatKind::Or:
            for (const Pat* a : static_cast<const OrPat*>(p)->alts) {
                if (irrefutable(a)) return true;
            }
            return false;
        default: return false;
    }
}

// Ключ «случая» для полноты: вариант перечисления, some/none, ok/err,
// true/false. -1 — образец не закрывает случай целиком.
int case_key(const Pat* p) {
    switch (p->kind) {
        case PatKind::Variant: {
            auto* v = static_cast<const VariantPat*>(p);
            if (v->res.kind != ResKind::Variant) return -1;
            for (const Pat* a : v->args) {
                if (!irrefutable(a)) return -1;
            }
            for (const FieldPat& f : v->fields) {
                if (f.pat && !irrefutable(f.pat)) return -1;
            }
            return static_cast<int>(v->res.index);
        }
        case PatKind::None: return 0;
        case PatKind::Wrap: {
            auto* w = static_cast<const WrapPat*>(p);
            if (!irrefutable(w->inner)) return -1;
            return w->which == WrapKind::Some ? 1 : w->which == WrapKind::Ok ? 0 : 1;
        }
        case PatKind::Lit: {
            auto* l = static_cast<const LitPat*>(p);
            if (auto* b = as<const BoolExpr>(l->lit)) return b->value ? 1 : 0;
            return -1;
        }
        default: return -1;
    }
}

void keys(const Pat* p, std::vector<int>& out) {
    if (p && p->kind == PatKind::Or) {
        for (const Pat* a : static_cast<const OrPat*>(p)->alts) keys(a, out);
    } else if (p) {
        out.push_back(case_key(p));
    }
}

}  // namespace

void Checker::exhaustive(MatchStmt* m, const Type* subject) {
    if (!subject || subject->kind == TK::Error) return;
    // Случаи конечного множества: имена для сообщения.
    std::vector<std::string> names;
    switch (subject->kind) {
        case TK::Enum:
            for (const VariantDef& v : subject->edef->variants) names.push_back("." + v.name);
            break;
        case TK::Optional: names = {"none", "some(_)"}; break;
        case TK::Result: names = {"ok(_)", "err(_)"}; break;
        case TK::Bool: names = {"false", "true"}; break;
        default: break;
    }
    std::set<int> covered;
    bool all = false;
    for (MatchArm& a : m->arms) {
        if (!a.pat || a.pat->kind == PatKind::Error) continue;
        if (all) {
            error(Code::E0416, a.pat->span, "this arm is unreachable: earlier arms cover every value");
            continue;
        }
        if (irrefutable(a.pat)) {
            all = true;
            continue;
        }
        if (names.empty()) continue;
        std::vector<int> ks;
        keys(a.pat, ks);
        bool adds = false, partial = false;
        for (int k : ks) {
            if (k < 0) partial = true;
            else if (!covered.count(k)) adds = true;
        }
        // Неполный образец (с условием внутри) может что-то добавить — не считаем лишним.
        if (!adds && !partial) {
            error(Code::E0416, a.pat->span, "this arm is unreachable: earlier arms cover these values");
        }
        for (int k : ks) {
            if (k >= 0) covered.insert(k);
        }
        if (covered.size() == names.size()) all = true;
    }
    if (all) return;
    if (names.empty()) {
        error(Code::E0415, m->subject->span, "'match' on '" + tn(subject) + "' needs an arm '_ => ...' for other values");
        return;
    }
    std::string missing;
    for (size_t i = 0; i < names.size(); i++) {
        if (!covered.count(static_cast<int>(i))) missing += (missing.empty() ? "" : ", ") + names[i];
    }
    error(Code::E0415, m->subject->span, "'match' does not cover " + missing);
}

}  // namespace dp::sema
