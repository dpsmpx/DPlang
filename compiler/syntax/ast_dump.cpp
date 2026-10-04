#include "syntax/ast_dump.h"

#include <cstdio>

#include "support/json.h"

namespace dp {

using namespace ast;

std::string_view bin_op_text(BinOp op) {
    switch (op) {
        case BinOp::Or: return "||";
        case BinOp::And: return "&&";
        case BinOp::Eq: return "==";
        case BinOp::Ne: return "!=";
        case BinOp::Lt: return "<";
        case BinOp::Le: return "<=";
        case BinOp::Gt: return ">";
        case BinOp::Ge: return ">=";
        case BinOp::Coalesce: return "??";
        case BinOp::Add: return "+";
        case BinOp::Sub: return "-";
        case BinOp::BitOr: return "|";
        case BinOp::BitXor: return "^";
        case BinOp::AddWrap: return "+%";
        case BinOp::SubWrap: return "-%";
        case BinOp::Mul: return "*";
        case BinOp::Div: return "/";
        case BinOp::Rem: return "%";
        case BinOp::Shl: return "<<";
        case BinOp::Shr: return ">>";
        case BinOp::BitAnd: return "&";
        case BinOp::MulWrap: return "*%";
    }
    return "?";
}

std::string_view assign_op_text(AssignOp op) {
    switch (op) {
        case AssignOp::Set: return "=";
        case AssignOp::Add: return "+=";
        case AssignOp::Sub: return "-=";
        case AssignOp::Mul: return "*=";
        case AssignOp::Div: return "/=";
        case AssignOp::Rem: return "%=";
        case AssignOp::BitAnd: return "&=";
        case AssignOp::BitOr: return "|=";
        case AssignOp::BitXor: return "^=";
        case AssignOp::Shl: return "<<=";
        case AssignOp::Shr: return ">>=";
    }
    return "?";
}

namespace {

std::string float_text(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

std::string lit_text(const Expr* e) {
    switch (e->kind) {
        case ExprKind::Int: return std::to_string(static_cast<const IntExpr*>(e)->value);
        case ExprKind::Float: return float_text(static_cast<const FloatExpr*>(e)->value);
        case ExprKind::Str: {
            std::string s;
            json_string(s, static_cast<const StrExpr*>(e)->value);
            return s;
        }
        case ExprKind::Byte: return "byte " + std::to_string(static_cast<const ByteExpr*>(e)->value);
        case ExprKind::Bool: return static_cast<const BoolExpr*>(e)->value ? "true" : "false";
        default: return "?";
    }
}

class Dumper {
public:
    std::string out;

    void line(int depth, const std::string& text) {
        out.append(static_cast<size_t>(depth) * 2, ' ');
        out += text;
        out += '\n';
    }

    void item(const Item* it, int d) {
        std::string pub = it->pub ? " pub" : "";
        switch (it->kind) {
            case ItemKind::Fn: {
                auto* f = static_cast<const FnItem*>(it);
                std::string attrs;
                for (const Attr& a : f->attrs) attrs += " @" + a.name;
                line(d, "Fn " + f->name + pub + attrs);
                for (const Param& p : f->params) {
                    std::string mode = p.mode == ParamMode::Mut ? "&mut " : p.mode == ParamMode::Ref ? "&" : "";
                    line(d + 1, "Param " + p.name + " " + mode + type_text(p.type));
                }
                if (f->ret) line(d + 1, "Ret " + type_text(f->ret));
                block(f->body, d + 1);
                break;
            }
            case ItemKind::Struct: {
                auto* s = static_cast<const StructItem*>(it);
                line(d, "Struct " + s->name + pub);
                for (const FieldDecl& f : s->fields) field(f, d + 1);
                break;
            }
            case ItemKind::Enum: {
                auto* e = static_cast<const EnumItem*>(it);
                line(d, "Enum " + e->name + pub);
                for (const VariantDecl& v : e->variants) {
                    std::string t = "Variant " + v.name;
                    if (v.form == VariantForm::Tuple) {
                        t += "(";
                        for (size_t i = 0; i < v.types.size(); i++) t += (i ? ", " : "") + type_text(v.types[i]);
                        t += ")";
                    } else if (v.form == VariantForm::Record) {
                        t += " {";
                        for (size_t i = 0; i < v.fields.size(); i++) {
                            t += (i ? ", " : "") + v.fields[i].name + ": " + type_text(v.fields[i].type);
                        }
                        t += "}";
                    }
                    line(d + 1, t);
                }
                break;
            }
            case ItemKind::Const: {
                auto* c = static_cast<const ConstItem*>(it);
                line(d, "Const " + c->name + pub + ": " + type_text(c->type));
                expr(c->value, d + 1);
                break;
            }
            case ItemKind::Import: line(d, "Import " + it->name); break;
            case ItemKind::Error: line(d, "ErrorItem"); break;
        }
    }

    void field(const FieldDecl& f, int d) {
        line(d, "Field " + f.name + ": " + type_text(f.type));
        if (f.value) expr(f.value, d + 1);
    }

    void block(const Block* b, int d) {
        if (!b) {
            line(d, "-");
            return;
        }
        line(d, "Block");
        for (const Stmt* s : b->stmts) stmt(s, d + 1);
    }

    void stmt(const Stmt* s, int d) {
        if (!s) {
            line(d, "-");
            return;
        }
        switch (s->kind) {
            case StmtKind::Let: {
                auto* l = static_cast<const LetStmt*>(s);
                line(d, std::string(l->is_var ? "Var " : "Let ") + l->name + (l->type ? ": " + type_text(l->type) : ""));
                if (l->value) expr(l->value, d + 1);
                break;
            }
            case StmtKind::Assign: {
                auto* a = static_cast<const AssignStmt*>(s);
                line(d, "Assign " + std::string(assign_op_text(a->op)));
                expr(a->target, d + 1);
                expr(a->value, d + 1);
                break;
            }
            case StmtKind::Expr: line(d, "ExprStmt"); expr(static_cast<const ExprStmt*>(s)->expr, d + 1); break;
            case StmtKind::If: {
                auto* i = static_cast<const IfStmt*>(s);
                if (i->cond) {
                    line(d, "If");
                    expr(i->cond, d + 1);
                } else {
                    line(d, "IfLet " + i->let_name);
                    expr(i->let_value, d + 1);
                }
                block(i->then, d + 1);
                if (i->els) {
                    line(d + 1, "Else");
                    stmt(i->els, d + 2);
                }
                break;
            }
            case StmtKind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                line(d, "While");
                expr(w->cond, d + 1);
                block(w->body, d + 1);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                std::string mode = f->mode == ForMode::Ref ? " &" : f->mode == ForMode::Mut ? " &mut"
                                                          : f->mode == ForMode::Range ? " range" : "";
                line(d, std::string(f->parallel ? "ParallelFor " : "For ") + f->var + " in" + mode);
                expr(f->iter, d + 1);
                if (f->hi) expr(f->hi, d + 1);
                block(f->body, d + 1);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<const MatchStmt*>(s);
                line(d, "Match");
                expr(m->subject, d + 1);
                for (const MatchArm& a : m->arms) {
                    line(d + 1, "Arm " + pat_text(a.pat));
                    stmt(a.body, d + 2);
                }
                break;
            }
            case StmtKind::Return: {
                line(d, "Return");
                if (auto* v = static_cast<const ReturnStmt*>(s)->value) expr(v, d + 1);
                break;
            }
            case StmtKind::Break: line(d, "Break"); break;
            case StmtKind::Continue: line(d, "Continue"); break;
            case StmtKind::Defer: line(d, "Defer"); stmt(static_cast<const DeferStmt*>(s)->body, d + 1); break;
            case StmtKind::Fail: line(d, "Fail"); expr(static_cast<const FailStmt*>(s)->value, d + 1); break;
            case StmtKind::Block: block(static_cast<const BlockStmt*>(s)->block, d); break;
            case StmtKind::Error: line(d, "ErrorStmt"); break;
        }
    }

    void field_inits(const std::vector<FieldInit>& fields, int d) {
        for (const FieldInit& f : fields) {
            line(d, "FieldInit " + f.name + (f.value ? "" : " (short)"));
            if (f.value) expr(f.value, d + 1);
        }
    }

    void expr(const Expr* e, int d) {
        if (!e) {
            line(d, "-");
            return;
        }
        switch (e->kind) {
            case ExprKind::Int: line(d, "Int " + lit_text(e)); break;
            case ExprKind::Float: line(d, "Float " + lit_text(e)); break;
            case ExprKind::Str: line(d, "Str " + lit_text(e)); break;
            case ExprKind::Byte: line(d, "Byte " + std::to_string(static_cast<const ByteExpr*>(e)->value)); break;
            case ExprKind::Bool: line(d, "Bool " + lit_text(e)); break;
            case ExprKind::None: line(d, "None"); break;
            case ExprKind::Name: line(d, "Name " + static_cast<const NameExpr*>(e)->name); break;
            case ExprKind::Field: {
                auto* f = static_cast<const FieldExpr*>(e);
                line(d, "Field ." + f->name);
                expr(f->object, d + 1);
                break;
            }
            case ExprKind::Index: {
                auto* i = static_cast<const IndexExpr*>(e);
                line(d, "Index");
                expr(i->object, d + 1);
                expr(i->index, d + 1);
                break;
            }
            case ExprKind::Slice: {
                auto* s = static_cast<const SliceExpr*>(e);
                line(d, "Slice");
                expr(s->object, d + 1);
                expr(s->lo, d + 1);
                expr(s->hi, d + 1);
                break;
            }
            case ExprKind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                line(d, "Call");
                expr(c->callee, d + 1);
                for (const Expr* a : c->args) expr(a, d + 1);
                break;
            }
            case ExprKind::Unary: {
                auto* u = static_cast<const UnaryExpr*>(e);
                line(d, std::string("Unary ") + (u->op == UnOp::Neg ? "-" : u->op == UnOp::Not ? "!" : "~"));
                expr(u->operand, d + 1);
                break;
            }
            case ExprKind::Binary: {
                auto* b = static_cast<const BinaryExpr*>(e);
                line(d, "Binary " + std::string(bin_op_text(b->op)));
                expr(b->lhs, d + 1);
                expr(b->rhs, d + 1);
                break;
            }
            case ExprKind::Try: line(d, "Try"); expr(static_cast<const TryExpr*>(e)->operand, d + 1); break;
            case ExprKind::Paren: line(d, "Paren"); expr(static_cast<const ParenExpr*>(e)->inner, d + 1); break;
            case ExprKind::Ref: {
                auto* r = static_cast<const RefExpr*>(e);
                line(d, r->mut ? "Ref mut" : "Ref");
                expr(r->operand, d + 1);
                break;
            }
            case ExprKind::StructLit: {
                auto* s = static_cast<const StructLitExpr*>(e);
                line(d, "StructLit");
                expr(s->path, d + 1);
                field_inits(s->fields, d + 1);
                break;
            }
            case ExprKind::VariantLit: {
                auto* v = static_cast<const VariantLitExpr*>(e);
                std::string form = v->form == VariantForm::Tuple ? "(..)" : v->form == VariantForm::Record ? " {..}" : "";
                line(d, "VariantLit ." + v->name + form);
                for (const Expr* a : v->args) expr(a, d + 1);
                field_inits(v->fields, d + 1);
                break;
            }
            case ExprKind::ListLit: {
                line(d, "List");
                for (const Expr* i : static_cast<const ListLitExpr*>(e)->items) expr(i, d + 1);
                break;
            }
            case ExprKind::Error: line(d, "ErrorExpr"); break;
        }
    }
};

}  // namespace

std::string type_text(const Type* t) {
    if (!t) return "-";
    switch (t->kind) {
        case TypeKind::Name: {
            auto* n = static_cast<const NameType*>(t);
            std::string s = n->module.empty() ? n->name : n->module + "." + n->name;
            if (!n->args.empty()) {
                s += "[";
                for (size_t i = 0; i < n->args.size(); i++) s += (i ? ", " : "") + type_text(n->args[i]);
                s += "]";
            }
            return s;
        }
        case TypeKind::Slice: return "[]" + type_text(static_cast<const SliceType*>(t)->elem);
        case TypeKind::Optional: return "?" + type_text(static_cast<const OptionalType*>(t)->inner);
        case TypeKind::Result: {
            auto* r = static_cast<const ResultType*>(t);
            return (r->error ? type_text(r->error) : "") + "!" + type_text(r->value);
        }
        case TypeKind::Error: return "<error>";
    }
    return "?";
}

std::string pat_text(const Pat* p) {
    if (!p) return "-";
    switch (p->kind) {
        case PatKind::Wild: return "_";
        case PatKind::Lit: {
            auto* l = static_cast<const LitPat*>(p);
            return (l->negative ? "-" : "") + lit_text(l->lit);
        }
        case PatKind::Bind: return static_cast<const BindPat*>(p)->name;
        case PatKind::None: return "none";
        case PatKind::Wrap: {
            auto* w = static_cast<const WrapPat*>(p);
            std::string n = w->which == WrapKind::Some ? "some" : w->which == WrapKind::Ok ? "ok" : "err";
            return n + "(" + pat_text(w->inner) + ")";
        }
        case PatKind::Variant: {
            auto* v = static_cast<const VariantPat*>(p);
            std::string s;
            for (const std::string& part : v->path) s += part + ".";
            s = v->path.empty() ? "." + v->name : s + v->name;
            if (v->form == VariantForm::Tuple) {
                s += "(";
                for (size_t i = 0; i < v->args.size(); i++) s += (i ? ", " : "") + pat_text(v->args[i]);
                s += ")";
            } else if (v->form == VariantForm::Record) {
                s += " {";
                for (size_t i = 0; i < v->fields.size(); i++) {
                    s += (i ? ", " : "") + v->fields[i].name;
                    if (v->fields[i].pat) s += ": " + pat_text(v->fields[i].pat);
                }
                s += "}";
            }
            return s;
        }
        case PatKind::Or: {
            auto* o = static_cast<const OrPat*>(p);
            std::string s;
            for (size_t i = 0; i < o->alts.size(); i++) s += (i ? " | " : "") + pat_text(o->alts[i]);
            return s;
        }
        case PatKind::Error: return "<error>";
    }
    return "?";
}

std::string dump_module(const Module& m) {
    Dumper d;
    d.line(0, "Module " + m.name);
    for (const Item* it : m.items) d.item(it, 1);
    return d.out;
}

}  // namespace dp
