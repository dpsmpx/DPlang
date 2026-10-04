#include "syntax/printer.h"

#include "syntax/ast_dump.h"

namespace dp {

namespace {

using namespace ast;

class Printer {
public:
    explicit Printer(std::string_view source) : src_(source) {}
    std::string out;

    void module(const Module& m) {
        const Item* prev = nullptr;
        for (const Item* it : m.items) {
            if (prev && !(prev->kind == ItemKind::Import && it->kind == ItemKind::Import)) out += '\n';
            item(it);
            prev = it;
        }
    }

private:
    std::string_view src_;

    std::string_view source_of(const Node* n) const { return src_.substr(n->span.begin, n->span.end - n->span.begin); }
    void pad(int d) { out.append(static_cast<size_t>(d) * 4, ' '); }

    void item(const Item* it) {
        std::string pub = it->pub ? "pub " : "";
        switch (it->kind) {
            case ItemKind::Fn: {
                auto* f = static_cast<const FnItem*>(it);
                for (const Attr& a : f->attrs) out += "@" + a.name + "\n";
                out += pub + "fn " + f->name + "(";
                for (size_t i = 0; i < f->params.size(); i++) {
                    const Param& p = f->params[i];
                    if (i) out += ", ";
                    out += p.name + ": ";
                    if (p.mode == ParamMode::Ref) out += "&";
                    if (p.mode == ParamMode::Mut) out += "&mut ";
                    out += type_text(p.type);
                }
                out += ")";
                if (f->ret) out += " " + type_text(f->ret);
                out += " ";
                block(f->body, 0);
                out += '\n';
                break;
            }
            case ItemKind::Struct: {
                auto* s = static_cast<const StructItem*>(it);
                out += pub + "struct " + s->name + " {\n";
                for (const FieldDecl& f : s->fields) {
                    pad(1);
                    field(f);
                    out += '\n';
                }
                out += "}\n";
                break;
            }
            case ItemKind::Enum: {
                auto* e = static_cast<const EnumItem*>(it);
                out += pub + "enum " + e->name + " {\n";
                for (const VariantDecl& v : e->variants) {
                    pad(1);
                    out += v.name;
                    if (v.form == VariantForm::Tuple) {
                        out += "(";
                        for (size_t i = 0; i < v.types.size(); i++) out += (i ? ", " : "") + type_text(v.types[i]);
                        out += ")";
                    } else if (v.form == VariantForm::Record) {
                        out += " { ";
                        for (size_t i = 0; i < v.fields.size(); i++) {
                            if (i) out += ", ";
                            field(v.fields[i]);
                        }
                        out += " }";
                    }
                    out += '\n';
                }
                out += "}\n";
                break;
            }
            case ItemKind::Const: {
                auto* c = static_cast<const ConstItem*>(it);
                out += pub + "const " + c->name + ": " + type_text(c->type) + " = ";
                expr(c->value);
                out += '\n';
                break;
            }
            case ItemKind::Import: out += "import " + it->name + "\n"; break;
            case ItemKind::Error: break;
        }
    }

    void field(const FieldDecl& f) {
        out += f.name + ": " + type_text(f.type);
        if (f.value) {
            out += " = ";
            expr(f.value);
        }
    }

    // Блок с текущей позиции: «{», инструкции с отступом d + 1, «}».
    void block(const Block* b, int d) {
        if (!b || b->stmts.empty()) {
            out += "{\n";
            pad(d);
            out += "}";
            return;
        }
        out += "{\n";
        for (const Stmt* s : b->stmts) {
            pad(d + 1);
            stmt(s, d + 1);
            out += '\n';
        }
        pad(d);
        out += "}";
    }

    void stmt(const Stmt* s, int d) {
        switch (s->kind) {
            case StmtKind::Let: {
                auto* l = static_cast<const LetStmt*>(s);
                out += std::string(l->is_var ? "var " : "let ") + l->name;
                if (l->type) out += ": " + type_text(l->type);
                if (l->value) {
                    out += " = ";
                    expr(l->value);
                }
                break;
            }
            case StmtKind::Assign: {
                auto* a = static_cast<const AssignStmt*>(s);
                expr(a->target);
                out += " " + std::string(assign_op_text(a->op)) + " ";
                expr(a->value);
                break;
            }
            case StmtKind::Expr: expr(static_cast<const ExprStmt*>(s)->expr); break;
            case StmtKind::If: if_stmt(static_cast<const IfStmt*>(s), d); break;
            case StmtKind::While: {
                auto* w = static_cast<const WhileStmt*>(s);
                out += "while ";
                expr(w->cond);
                out += " ";
                block(w->body, d);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                out += std::string(f->parallel ? "parallel for " : "for ") + f->var + " in ";
                if (f->mode == ForMode::Ref) out += "&";
                if (f->mode == ForMode::Mut) out += "&mut ";
                expr(f->iter);
                if (f->mode == ForMode::Range) {
                    out += "..";
                    expr(f->hi);
                }
                out += " ";
                block(f->body, d);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<const MatchStmt*>(s);
                out += "match ";
                expr(m->subject);
                out += " {\n";
                for (const MatchArm& a : m->arms) {
                    pad(d + 1);
                    pat(a.pat);
                    out += " => ";
                    stmt(a.body, d + 1);
                    out += '\n';
                }
                pad(d);
                out += "}";
                break;
            }
            case StmtKind::Return: {
                out += "return";
                if (auto* v = static_cast<const ReturnStmt*>(s)->value) {
                    out += " ";
                    expr(v);
                }
                break;
            }
            case StmtKind::Break: out += "break"; break;
            case StmtKind::Continue: out += "continue"; break;
            case StmtKind::Defer: out += "defer "; stmt(static_cast<const DeferStmt*>(s)->body, d); break;
            case StmtKind::Fail: out += "fail "; expr(static_cast<const FailStmt*>(s)->value); break;
            case StmtKind::Block: block(static_cast<const BlockStmt*>(s)->block, d); break;
            case StmtKind::Error: break;
        }
    }

    void if_stmt(const IfStmt* i, int d) {
        out += "if ";
        if (i->cond) {
            expr(i->cond);
        } else {
            out += "let " + i->let_name + " = ";
            expr(i->let_value);
        }
        out += " ";
        block(i->then, d);
        if (i->els) {
            out += " else ";
            if (i->els->kind == StmtKind::If) if_stmt(static_cast<const IfStmt*>(i->els), d);
            else block(static_cast<const BlockStmt*>(i->els)->block, d);
        }
    }

    void pat(const Pat* p) {
        switch (p->kind) {
            case PatKind::Wild: out += "_"; break;
            case PatKind::Lit: {
                auto* l = static_cast<const LitPat*>(p);
                if (l->negative) out += "-";
                expr(l->lit);
                break;
            }
            case PatKind::Bind: out += static_cast<const BindPat*>(p)->name; break;
            case PatKind::None: out += "none"; break;
            case PatKind::Wrap: {
                auto* w = static_cast<const WrapPat*>(p);
                out += w->which == WrapKind::Some ? "some(" : w->which == WrapKind::Ok ? "ok(" : "err(";
                pat(w->inner);
                out += ")";
                break;
            }
            case PatKind::Variant: {
                auto* v = static_cast<const VariantPat*>(p);
                if (v->path.empty()) out += ".";
                for (const std::string& part : v->path) out += part + ".";
                out += v->name;
                if (v->form == VariantForm::Tuple) {
                    out += "(";
                    for (size_t i = 0; i < v->args.size(); i++) {
                        if (i) out += ", ";
                        pat(v->args[i]);
                    }
                    out += ")";
                } else if (v->form == VariantForm::Record) {
                    out += " {";
                    for (size_t i = 0; i < v->fields.size(); i++) {
                        out += i ? ", " : " ";
                        out += v->fields[i].name;
                        if (v->fields[i].pat) {
                            out += ": ";
                            pat(v->fields[i].pat);
                        }
                    }
                    out += v->fields.empty() ? "}" : " }";
                }
                break;
            }
            case PatKind::Or: {
                auto* o = static_cast<const OrPat*>(p);
                for (size_t i = 0; i < o->alts.size(); i++) {
                    if (i) out += " | ";
                    pat(o->alts[i]);
                }
                break;
            }
            case PatKind::Error: break;
        }
    }

    void exprs(const std::vector<Expr*>& list) {
        for (size_t i = 0; i < list.size(); i++) {
            if (i) out += ", ";
            expr(list[i]);
        }
    }

    void field_inits(const std::vector<FieldInit>& fields) {
        out += " {";
        for (size_t i = 0; i < fields.size(); i++) {
            out += i ? ", " : " ";
            out += fields[i].name;
            if (fields[i].value) {
                out += ": ";
                expr(fields[i].value);
            }
        }
        out += fields.empty() ? "}" : " }";
    }

    void expr(const Expr* e) {
        if (!e) return;
        switch (e->kind) {
            case ExprKind::Int: case ExprKind::Float: case ExprKind::Str: case ExprKind::Byte:
                out += source_of(e);
                break;
            case ExprKind::Bool: out += static_cast<const BoolExpr*>(e)->value ? "true" : "false"; break;
            case ExprKind::None: out += "none"; break;
            case ExprKind::Name: out += static_cast<const NameExpr*>(e)->name; break;
            case ExprKind::Field: {
                auto* f = static_cast<const FieldExpr*>(e);
                expr(f->object);
                out += "." + f->name;
                break;
            }
            case ExprKind::Index: {
                auto* i = static_cast<const IndexExpr*>(e);
                expr(i->object);
                out += "[";
                expr(i->index);
                out += "]";
                break;
            }
            case ExprKind::Slice: {
                auto* s = static_cast<const SliceExpr*>(e);
                expr(s->object);
                out += "[";
                expr(s->lo);
                out += "..";
                expr(s->hi);
                out += "]";
                break;
            }
            case ExprKind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                expr(c->callee);
                out += "(";
                exprs(c->args);
                out += ")";
                break;
            }
            case ExprKind::Unary: {
                auto* u = static_cast<const UnaryExpr*>(e);
                out += u->op == UnOp::Neg ? "-" : u->op == UnOp::Not ? "!" : "~";
                expr(u->operand);
                break;
            }
            case ExprKind::Binary: {
                auto* b = static_cast<const BinaryExpr*>(e);
                expr(b->lhs);
                out += " " + std::string(bin_op_text(b->op)) + " ";
                expr(b->rhs);
                break;
            }
            case ExprKind::Try: out += "try "; expr(static_cast<const TryExpr*>(e)->operand); break;
            case ExprKind::Paren:
                out += "(";
                expr(static_cast<const ParenExpr*>(e)->inner);
                out += ")";
                break;
            case ExprKind::Ref: {
                auto* r = static_cast<const RefExpr*>(e);
                out += r->mut ? "&mut " : "&";
                expr(r->operand);
                break;
            }
            case ExprKind::StructLit: {
                auto* s = static_cast<const StructLitExpr*>(e);
                expr(s->path);
                field_inits(s->fields);
                break;
            }
            case ExprKind::VariantLit: {
                auto* v = static_cast<const VariantLitExpr*>(e);
                out += "." + v->name;
                if (v->form == VariantForm::Tuple) {
                    out += "(";
                    exprs(v->args);
                    out += ")";
                } else if (v->form == VariantForm::Record) {
                    field_inits(v->fields);
                }
                break;
            }
            case ExprKind::ListLit:
                out += "[";
                exprs(static_cast<const ListLitExpr*>(e)->items);
                out += "]";
                break;
            case ExprKind::Error: break;
        }
    }
};

}  // namespace

std::string print_module(const Module& m, std::string_view source) {
    Printer p(source);
    p.module(m);
    return p.out;
}

}  // namespace dp
