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
// `debug parse` и фаззер. Строки — до 100 символов: выражение и параметры
// функции раскладываются документом с группами (Doc, render).
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
                    out += f->attrs[i].text() + "\n";
                }
                out += pub + (f->is_extern ? "extern fn " : "fn ") + f->name;
                // Параметры не влезают в строку — по одному на строку.
                std::vector<Doc> ps;
                for (const Param& p : f->params) {
                    std::string mode = p.mode == ParamMode::Ref ? "&" : p.mode == ParamMode::Mut ? "&mut " : "";
                    ps.push_back(text(p.name + ": " + mode + type_text(p.type)));
                }
                std::string ret = f->ret ? " " + type_text(f->ret) : "";
                render(bracket("(", std::move(ps), ")"), width(ret) + (f->body ? 2 : 0));
                out += ret;
                if (!f->body) break;  // extern fn — без тела
                out += " ";
                block(f->body, 0);
                break;
            }
            case ItemKind::ExternType:
                for (const Attr& a : static_cast<const ExternTypeItem*>(it)->attrs) out += a.text() + "\n";
                out += pub + "extern type " + it->name;
                break;
            case ItemKind::Struct: {
                auto* s = static_cast<const StructItem*>(it);
                for (const Attr& a : s->attrs) out += a.text() + "\n";
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
        for (const Attr& a : f.attrs) out += a.text() + " ";
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
                if (l->destructure) {
                    out += " {";
                    for (size_t i = 0; i < l->fields.size(); i++) {
                        const LetField& f = l->fields[i];
                        out += (i ? ", " : " ") + f.field;
                        if (f.name != f.field) out += ": " + f.name;
                    }
                    out += l->fields.empty() ? "}" : " }";
                }
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
                expr(w->cond, 2);
                out += " ";
                block(w->body, d);
                break;
            }
            case StmtKind::For: {
                auto* f = static_cast<const ForStmt*>(s);
                out += std::string(f->parallel ? "parallel for " : "for ") + f->var + " in ";
                if (f->mode == ForMode::Ref) out += "&";
                if (f->mode == ForMode::Mut) out += "&mut ";
                expr(f->iter, f->mode == ForMode::Range ? 0 : 2);
                if (f->mode == ForMode::Range) {
                    out += "..";
                    expr(f->hi, 2);
                }
                out += " ";
                block(f->body, d);
                break;
            }
            case StmtKind::Match: {
                auto* m = static_cast<const MatchStmt*>(s);
                out += "match ";
                expr(m->subject, 2);
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
            case StmtKind::Lock: {
                auto* l = static_cast<const LockStmt*>(s);
                out += "lock ";
                expr(l->mutex);
                out += " as " + l->var + " ";
                block(l->body, d);
                break;
            }
            case StmtKind::Tasks: {
                auto* t = static_cast<const TasksStmt*>(s);
                out += "tasks ";
                block(t->body, d);
                break;
            }
            case StmtKind::Defer: {
                auto* df = static_cast<const DeferStmt*>(s);
                out += df->on_error ? "errdefer " : "defer ";
                stmt(df->body, d);
                break;
            }
            case StmtKind::Fail: out += "fail "; expr(static_cast<const FailStmt*>(s)->value); break;
            case StmtKind::Block: block(static_cast<const BlockStmt*>(s)->block, d); break;
            case StmtKind::Error: break;
        }
    }

    void if_stmt(const IfStmt* i, int d) {
        out += "if ";
        if (i->cond) {
            expr(i->cond, 2);
        } else {
            out += "let " + i->let_name + " = ";
            expr(i->let_value, 2);
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

    // --- выражения: документ и раскладка в ширину kWidth
    //
    // Выражение строится документом (текст, перенос, группа, отступ) и
    // раскладывается с текущей колонки: группа печатается в строку, если
    // влезает до следующего возможного переноса, иначе её переносы — концы
    // строк. Переносятся списки (аргументы вызова, элементы списка, поля
    // литерала, данные варианта — по одному на строку с отступом +4) и
    // бинарные операторы (после оператора, продолжение с отступом +4):
    // там конец строки выражения не кончает (syntax.newline).
    static constexpr int kWidth = 100;
    static constexpr size_t kSep = SIZE_MAX;  // Cmd.pos у переноса Fill

    struct Doc {
        enum class K : uint8_t { Text, Line, Group, Nest, Cat, Fill } k = K::Cat;  // Fill: часть, перенос, часть…
        std::string s;          // Text — текст; Line — текст в строку
        std::vector<Doc> kids;  // Group, Nest, Cat
    };
    static Doc text(std::string s) { return Doc{Doc::K::Text, std::move(s), {}}; }
    static Doc line(std::string flat) { return Doc{Doc::K::Line, std::move(flat), {}}; }
    static Doc wrap(Doc::K k, Doc d) {
        Doc r{k, {}, {}};
        r.kids.push_back(std::move(d));
        return r;
    }
    static Doc group(Doc d) { return wrap(Doc::K::Group, std::move(d)); }
    static Doc nest(Doc d) { return wrap(Doc::K::Nest, std::move(d)); }
    static Doc cat(std::vector<Doc> ds) { return Doc{Doc::K::Cat, {}, std::move(ds)}; }
    // Части переносятся, а не копируются: у глубокой вложенности копии
    // поддеревьев на каждом уровне дали бы квадратичное время.
    template <class... D>
    static Doc catv(D&&... ds) {
        Doc r{Doc::K::Cat, {}, {}};
        r.kids.reserve(sizeof...(ds));
        (r.kids.push_back(std::forward<D>(ds)), ...);
        return r;
    }
    static Doc fill(std::vector<Doc> ds) { return Doc{Doc::K::Fill, {}, std::move(ds)}; }

    // Ширина строки в символах (UTF-8).
    static int width(std::string_view s) {
        int n = 0;
        for (unsigned char c : s) n += (c & 0xC0) != 0x80;
        return n;
    }

    // Список в скобках: в строку — `open a, b close`, иначе по элементу на
    // строку. pad — пробел у скобок в строку (литерал структуры).
    static Doc bracket(const std::string& open, std::vector<Doc> items, const std::string& close, bool pad = false) {
        if (items.empty()) return text(open + close);
        std::vector<Doc> in;
        in.push_back(line(pad ? " " : ""));
        for (size_t i = 0; i < items.size(); i++) {
            if (i) {
                in.push_back(text(","));
                in.push_back(line(" "));
            }
            in.push_back(std::move(items[i]));
        }
        return group(catv(text(open), nest(cat(std::move(in))), line(pad ? " " : ""), text(close)));
    }

    std::vector<Doc> docs(const std::vector<Expr*>& list) {
        std::vector<Doc> out_;
        for (const Expr* e : list) out_.push_back(doc(e));
        return out_;
    }

    Doc field_inits(const std::vector<FieldInit>& fields) {
        std::vector<Doc> items;
        for (const FieldInit& f : fields) {
            if (f.value) items.push_back(catv(text(f.name + ": "), doc(f.value)));
            else items.push_back(text(f.name));
        }
        return catv(text(" "), bracket("{", std::move(items), "}", true));
    }

    Doc doc(const Expr* e) {
        if (!e) return text("");
        switch (e->kind) {
            case ExprKind::Int: case ExprKind::Float: case ExprKind::Str: case ExprKind::Byte:
                return text(std::string(source_of(e)));
            case ExprKind::Bool: return text(static_cast<const BoolExpr*>(e)->value ? "true" : "false");
            case ExprKind::None: return text("none");
            case ExprKind::Name: return text(static_cast<const NameExpr*>(e)->name);
            case ExprKind::Field: {
                auto* f = static_cast<const FieldExpr*>(e);
                return catv(doc(f->object), text("." + f->name));
            }
            case ExprKind::Index: {
                auto* i = static_cast<const IndexExpr*>(e);
                return catv(doc(i->object), text("["), doc(i->index), text("]"));
            }
            case ExprKind::Slice: {
                auto* s = static_cast<const SliceExpr*>(e);
                return catv(doc(s->object), text("["), doc(s->lo), text(".."), doc(s->hi), text("]"));
            }
            case ExprKind::Call: {
                auto* c = static_cast<const CallExpr*>(e);
                return catv(doc(c->callee), bracket("(", docs(c->args), ")"));
            }
            case ExprKind::Unary: {
                auto* u = static_cast<const UnaryExpr*>(e);
                return catv(text(u->op == UnOp::Neg ? "-" : u->op == UnOp::Not ? "!" : "~"), doc(u->operand));
            }
            case ExprKind::Binary: {
                // Цепочка одного оператора `a + b + c` (левая): на строку —
                // сколько влезает, оператор в конце строки.
                auto* b = static_cast<const BinaryExpr*>(e);
                std::vector<const Expr*> ops{b->rhs};
                const Expr* head = b->lhs;
                while (auto* l = as<const BinaryExpr>(head)) {
                    if (l->op != b->op) break;
                    ops.push_back(l->rhs);
                    head = l->lhs;
                }
                ops.push_back(head);
                std::string op = " " + std::string(bin_op_text(b->op));
                std::vector<Doc> parts;
                for (size_t i = ops.size(); i-- > 0;) {
                    if (!parts.empty()) parts.push_back(line(" "));
                    parts.push_back(i ? catv(doc(ops[i]), text(op)) : doc(ops[i]));
                }
                return nest(fill(std::move(parts)));
            }
            case ExprKind::Try: return catv(text("try "), doc(static_cast<const TryExpr*>(e)->operand));
            case ExprKind::Spawn: return catv(text("spawn "), doc(static_cast<const SpawnExpr*>(e)->operand));
            case ExprKind::Paren: return catv(text("("), doc(static_cast<const ParenExpr*>(e)->inner), text(")"));
            case ExprKind::Ref: {
                auto* r = static_cast<const RefExpr*>(e);
                return catv(text(r->mut ? "&mut " : "&"), doc(r->operand));
            }
            case ExprKind::StructLit: {
                auto* s = static_cast<const StructLitExpr*>(e);
                return catv(doc(s->path), field_inits(s->fields));
            }
            case ExprKind::VariantLit: {
                auto* v = static_cast<const VariantLitExpr*>(e);
                if (v->form == VariantForm::Tuple) return catv(text("." + v->name), bracket("(", docs(v->args), ")"));
                if (v->form == VariantForm::Record) return catv(text("." + v->name), field_inits(v->fields));
                return text("." + v->name);
            }
            case ExprKind::ListLit: return bracket("[", docs(static_cast<const ListLitExpr*>(e)->items), "]");
            case ExprKind::Error: return text("");
        }
        return text("");
    }

    // Раскладка: команды — (отступ, в строку, документ).
    struct Cmd {
        int indent;
        bool flat;
        const Doc* d;
        size_t pos = 0;  // Fill: номер следующей части
    };

    // Влезает ли next в строку: от колонки до первого переноса остальных
    // команд (rest — в их режимах), с хвостом trail после выражения.
    static bool fits(std::vector<Cmd> st, const std::vector<Cmd>& rest, size_t r, int left) {
        while (left >= 0) {
            if (st.empty()) {
                if (r == 0) return true;
                st.push_back(rest[--r]);
                continue;
            }
            Cmd c = st.back();
            st.pop_back();
            switch (c.d->k) {
                case Doc::K::Text: left -= width(c.d->s); break;
                case Doc::K::Line:
                    if (!c.flat) return true;
                    left -= width(c.d->s);
                    break;
                case Doc::K::Group: st.push_back({c.indent, c.flat, &c.d->kids[0]}); break;
                case Doc::K::Nest: st.push_back({c.indent + 4, c.flat, &c.d->kids[0]}); break;
                case Doc::K::Cat: case Doc::K::Fill:
                    for (auto it = c.d->kids.rbegin(); it != c.d->kids.rend(); ++it) st.push_back({c.indent, c.flat, &*it});
                    break;
            }
        }
        return false;
    }

    // Выражение с текущей позиции out; trail — ширина текста после него на
    // той же строке (` {` у заголовка блока).
    void expr(const Expr* e, int trail = 0) {
        if (e) render(doc(e), trail);
    }

    void render(const Doc& d, int trail) {
        size_t nl = out.rfind('\n');
        size_t line_start = nl == std::string::npos ? 0 : nl + 1;
        int col = width(std::string_view(out).substr(line_start));
        int base = 0;
        while (line_start + static_cast<size_t>(base) < out.size() && out[line_start + static_cast<size_t>(base)] == ' ') base++;
        Doc tail = text(std::string(static_cast<size_t>(trail), ' '));
        std::vector<Cmd> st{{base, true, &tail}, {base, false, &d}};
        while (!st.empty()) {
            Cmd c = st.back();
            st.pop_back();
            if (c.pos == kSep) {  // перенос Fill: в строку, если следующая часть влезает
                const Cmd& next = st.back();
                const Doc& nd = next.d->kids[next.pos];
                c.flat = c.flat || fits({{c.indent, true, &nd}, {c.indent, true, c.d}}, st, st.size() - 1, kWidth - col);
            }
            switch (c.d->k) {
                case Doc::K::Text:
                    if (c.d != &tail) {
                        out += c.d->s;
                        col += width(c.d->s);
                    }
                    break;
                case Doc::K::Line:
                    if (c.flat) {
                        out += c.d->s;
                        col += width(c.d->s);
                    } else {
                        out += '\n';
                        out.append(static_cast<size_t>(c.indent), ' ');
                        col = c.indent;
                    }
                    break;
                case Doc::K::Group: {
                    // Отступ у края ширины: перенос ничего не даёт, а у глубокой
                    // вложенности дал бы квадратичный вывод — в строку.
                    Cmd flat{c.indent, true, &c.d->kids[0]};
                    bool deep = c.indent >= kWidth - 20;
                    st.push_back(c.flat || deep || fits({flat}, st, st.size(), kWidth - col) ? flat
                                                                                          : Cmd{c.indent, false, &c.d->kids[0]});
                    break;
                }
                case Doc::K::Nest: st.push_back({c.indent + 4, c.flat, &c.d->kids[0]}); break;
                case Doc::K::Cat:
                    for (auto it = c.d->kids.rbegin(); it != c.d->kids.rend(); ++it) st.push_back({c.indent, c.flat, &*it});
                    break;
                case Doc::K::Fill: {
                    // Часть pos; перед следующей частью перенос — только если
                    // она с переносом в строку не влезает.
                    const auto& k = c.d->kids;
                    if (c.pos >= k.size()) break;
                    Cmd part{c.indent, c.flat, &k[c.pos]};
                    if (c.pos + 1 < k.size()) {
                        st.push_back({c.indent, c.flat, c.d, c.pos + 2});
                        st.push_back({c.indent, c.flat, &k[c.pos + 1], kSep});
                    }
                    bool flat = c.flat || c.indent >= kWidth - 20 || fits({{c.indent, true, part.d}}, st, st.size(), kWidth - col);
                    st.push_back(flat ? Cmd{c.indent, true, part.d} : part);
                    break;
                }
            }
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
