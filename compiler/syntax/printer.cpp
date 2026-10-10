// Каноническая печать модуля — `dpl fmt` (docs/compiler/cli.md, «fmt»).
//
// Код печатается по AST в одном формате (Р7). Комментарии и пустые
// строки берутся из токенов дерева без потерь:
// - комментарий на своей строке стоит перед следующим элементом своего
//   контейнера (объявление, поле, вариант, инструкция, ветвь match) или
//   в конце контейнера, с отступом элементов;
// - комментарий после кода — в конце той же строки: строки элемента или
//   открывающей скобки;
// - комментарий внутри элемента, но вне вложенных блоков (например, в
//   многострочном выражении), печатается своей строкой перед элементом;
// - пустые строки между элементами сохраняются, не больше одной подряд;
//   между объявлениями модуля пустая строка есть всегда (кроме
//   соседних import).
// Ни один комментарий не теряется и не повторяется: это проверяет круг
// `debug parse` и фаззер.
#include "syntax/printer.h"

#include "syntax/ast_dump.h"

namespace dp {

namespace {

using namespace ast;

struct Comment {
    uint32_t begin = 0, end = 0;
    bool printed = false;
};

class Printer {
public:
    Printer(const Module& m, std::string_view source) : m_(m), src_(source) {
        for (const Token& t : m.tokens) {
            if (t.kind == TokKind::Comment) comments_.push_back(Comment{t.begin, t.end});
        }
    }
    std::string out;

    void module() {
        const auto& items = m_.items;
        Lines lines{0, LinesMode::Module};
        for (size_t i = 0; i < items.size(); i++) {
            const Item* it = items[i];
            bool blank = i > 0 && !(items[i - 1]->kind == ItemKind::Import && it->kind == ItemKind::Import);
            element(lines, it->span, blank, [&] { item(it); });
        }
        tail(lines, static_cast<uint32_t>(src_.size()));
    }

private:
    const Module& m_;
    std::string_view src_;
    std::vector<Comment> comments_;

    std::string_view source_of(const Node* n) const { return src_.substr(n->span.begin, n->span.end - n->span.begin); }
    void pad(int d) { out.append(static_cast<size_t>(d) * 4, ' '); }

    // --- комментарии и пустые строки

    std::string_view comment_text(const Comment& c) const {
        std::string_view t = src_.substr(c.begin, c.end - c.begin);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r')) t.remove_suffix(1);
        return t;
    }

    // Между a и b в исходнике есть пустая строка.
    bool blank_between(uint32_t a, uint32_t b) const {
        int newlines = 0;
        for (uint32_t i = a; i < b && i < src_.size(); i++) {
            char c = src_[i];
            if (c == '\n') {
                if (++newlines == 2) return true;
            } else if (c != ' ' && c != '\t' && c != '\r') {
                newlines = 0;
            }
        }
        return false;
    }

    bool same_line(uint32_t a, uint32_t b) const {
        if (a > b) std::swap(a, b);
        for (uint32_t i = a; i < b && i < src_.size(); i++) {
            if (src_[i] == '\n') return false;
        }
        return true;
    }

    // Комментарии после кода на строке, где кончается offset (до конца
    // строки): печатаются в конце текущей строки вывода.
    void trailing(uint32_t offset, uint32_t& last_end) {
        for (Comment& c : comments_) {
            if (c.printed || c.begin < offset) continue;
            if (!same_line(offset, c.begin)) break;
            out += " ";
            out += comment_text(c);
            c.printed = true;
            last_end = c.end;
        }
    }

    enum class LinesMode : uint8_t { Module, Lines };

    // Ряд элементов контейнера: отступ, режим и конец последнего
    // напечатанного элемента (для пустых строк).
    struct Lines {
        int indent = 0;
        LinesMode mode = LinesMode::Lines;
        uint32_t from = 0;  // начало контейнера: комментарии раньше — не его
        bool any = false;
        uint32_t last_end = 0;
    };

    // Комментарии своими строками перед offset (ведущие).
    void own_line(Lines& l, uint32_t upto, bool blank_first) {
        for (Comment& c : comments_) {
            if (c.printed || c.begin < l.from || c.begin >= upto) continue;
            bool blank = l.any && (blank_between(l.last_end, c.begin) || blank_first);
            blank_first = false;
            if (blank) out += '\n';
            pad(l.indent);
            out += comment_text(c);
            out += '\n';
            c.printed = true;
            l.any = true;
            l.last_end = c.end;
        }
    }

    // Элемент контейнера: ведущие комментарии, комментарии внутри него вне
    // вложенных блоков, сам элемент, комментарий в конце его строки.
    template <class F>
    void element(Lines& l, Span span, bool blank_rule, F print) {
        bool had_lead = false;
        for (const Comment& c : comments_) had_lead = had_lead || (!c.printed && c.begin >= l.from && c.begin < span.begin);
        // Пустая строка по правилу модуля — перед ведущими комментариями.
        own_line(l, span.begin, blank_rule && l.mode == LinesMode::Module);
        std::string saved;
        saved.swap(out);
        pad(l.indent);
        print();
        std::string body;
        body.swap(out);
        out.swap(saved);
        bool blank = l.any && (blank_between(l.last_end, span.begin) ||
                               (blank_rule && l.mode == LinesMode::Module && !had_lead));
        bool strays = false;
        for (Comment& c : comments_) {
            if (c.printed || c.begin < span.begin || c.begin >= span.end) continue;
            if (!strays && blank) out += '\n';
            strays = true;
            pad(l.indent);
            out += comment_text(c);
            out += '\n';
            c.printed = true;
        }
        if (blank && !strays) out += '\n';
        out += body;
        l.last_end = span.end;
        trailing(span.end, l.last_end);
        out += '\n';
        l.any = true;
    }

    // Комментарии в конце контейнера (до закрывающей скобки или конца файла).
    void tail(Lines& l, uint32_t close) { own_line(l, close, false); }

    // Комментарий на строке открывающей скобки.
    void after_open(uint32_t brace, Lines& l) {
        l.from = brace + 1;
        l.last_end = brace + 1;
        trailing(brace + 1, l.last_end);
    }

    // Первая «{» в участке [from, to) исходника — по токенам.
    uint32_t brace_after(uint32_t from, uint32_t to) const {
        for (const Token& t : m_.tokens) {
            if (t.begin >= from && t.begin < to && t.kind == TokKind::LBrace) return t.begin;
        }
        return from;
    }

    // --- объявления

    void item(const Item* it) {
        std::string pub = it->pub ? "pub " : "";
        switch (it->kind) {
            case ItemKind::Fn: {
                auto* f = static_cast<const FnItem*>(it);
                for (size_t i = 0; i < f->attrs.size(); i++) {
                    if (i) pad(0);
                    out += "@" + f->attrs[i].name + "\n";
                }
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
                break;
            }
            case ItemKind::Struct: {
                auto* s = static_cast<const StructItem*>(it);
                out += pub + "struct " + s->name + " {";
                Lines l{1, LinesMode::Lines};
                after_open(brace_after(s->name_span.end, s->span.end), l);
                out += '\n';
                for (const FieldDecl& f : s->fields) element(l, f.span, false, [&] { field(f); });
                tail(l, s->span.end);
                out += "}";
                break;
            }
            case ItemKind::Enum: {
                auto* e = static_cast<const EnumItem*>(it);
                out += pub + "enum " + e->name + " {";
                Lines l{1, LinesMode::Lines};
                after_open(brace_after(e->name_span.end, e->span.end), l);
                out += '\n';
                for (const VariantDecl& v : e->variants) element(l, v.span, false, [&] { variant(v); });
                tail(l, e->span.end);
                out += "}";
                break;
            }
            case ItemKind::Const: {
                auto* c = static_cast<const ConstItem*>(it);
                out += pub + "const " + c->name + ": " + type_text(c->type) + " = ";
                expr(c->value);
                break;
            }
            case ItemKind::Import:
                for (const Attr& a : static_cast<const ImportItem*>(it)->attrs) out += "@" + a.name + " ";
                out += "import " + it->name;
                break;
            case ItemKind::Error: break;
        }
    }

    void variant(const VariantDecl& v) {
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
    }

    void field(const FieldDecl& f) {
        out += f.name + ": " + type_text(f.type);
        if (f.value) {
            out += " = ";
            expr(f.value);
        }
    }

    // Блок с текущей позиции: «{», инструкции с отступом d + 1, «}».
    void block(const ast::Block* b, int d) {
        bool empty = !b || b->stmts.empty();
        for (const Comment& c : comments_) {
            if (b && !c.printed && c.begin > b->span.begin && c.begin < b->span.end) empty = false;
        }
        if (empty) {  // пустой блок без комментариев — `{}`
            out += "{}";
            return;
        }
        out += "{";
        Lines l{d + 1, LinesMode::Lines};
        if (b) after_open(b->span.begin, l);
        out += '\n';
        if (b) {
            for (const Stmt* s : b->stmts) element(l, s->span, false, [&] { stmt(s, d + 1); });
            tail(l, b->span.end > 0 ? b->span.end - 1 : 0);
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
                out += " {";
                Lines l{d + 1, LinesMode::Lines};
                after_open(brace_after(m->subject ? m->subject->span.end : m->span.begin, m->span.end), l);
                out += '\n';
                for (const MatchArm& a : m->arms) {
                    element(l, a.span, false, [&] {
                        pat(a.pat);
                        out += " => ";
                        stmt(a.body, d + 1);
                    });
                }
                tail(l, m->span.end > 0 ? m->span.end - 1 : 0);
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
    Printer p(m, source);
    p.module();
    return p.out;
}

std::vector<std::string> comment_texts(const Module& m, std::string_view source) {
    std::vector<std::string> out;
    for (const Token& t : m.tokens) {
        if (t.kind != TokKind::Comment) continue;
        std::string_view s = source.substr(t.begin, t.end - t.begin);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        out.emplace_back(s);
    }
    return out;
}

}  // namespace dp
