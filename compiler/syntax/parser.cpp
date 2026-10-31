#include "syntax/parser.h"

#include "syntax/literal.h"

namespace dp {

namespace {

using namespace ast;

// Приоритет бинарного оператора (docs/spec/core.md, expr.prec); 0 — не бинарный.
int prec(TokKind k) {
    switch (k) {
        case TokKind::OrOr: return 1;
        case TokKind::AndAnd: return 2;
        case TokKind::EqEq: case TokKind::NotEq: case TokKind::Lt: case TokKind::LtEq: case TokKind::Gt:
        case TokKind::GtEq: return 3;
        case TokKind::QQ: return 4;
        case TokKind::Plus: case TokKind::Minus: case TokKind::Pipe: case TokKind::Caret: case TokKind::PlusWrap:
        case TokKind::MinusWrap: return 5;
        case TokKind::Star: case TokKind::Slash: case TokKind::Percent: case TokKind::Shl: case TokKind::Shr:
        case TokKind::Amp: case TokKind::StarWrap: return 6;
        default: return 0;
    }
}

BinOp bin_op(TokKind k) {
    switch (k) {
        case TokKind::OrOr: return BinOp::Or;
        case TokKind::AndAnd: return BinOp::And;
        case TokKind::EqEq: return BinOp::Eq;
        case TokKind::NotEq: return BinOp::Ne;
        case TokKind::Lt: return BinOp::Lt;
        case TokKind::LtEq: return BinOp::Le;
        case TokKind::Gt: return BinOp::Gt;
        case TokKind::GtEq: return BinOp::Ge;
        case TokKind::QQ: return BinOp::Coalesce;
        case TokKind::Plus: return BinOp::Add;
        case TokKind::Minus: return BinOp::Sub;
        case TokKind::Pipe: return BinOp::BitOr;
        case TokKind::Caret: return BinOp::BitXor;
        case TokKind::PlusWrap: return BinOp::AddWrap;
        case TokKind::MinusWrap: return BinOp::SubWrap;
        case TokKind::Star: return BinOp::Mul;
        case TokKind::Slash: return BinOp::Div;
        case TokKind::Percent: return BinOp::Rem;
        case TokKind::Shl: return BinOp::Shl;
        case TokKind::Shr: return BinOp::Shr;
        case TokKind::Amp: return BinOp::BitAnd;
        default: return BinOp::MulWrap;
    }
}

bool assign_op(TokKind k, AssignOp& op) {
    switch (k) {
        case TokKind::Eq: op = AssignOp::Set; return true;
        case TokKind::PlusEq: op = AssignOp::Add; return true;
        case TokKind::MinusEq: op = AssignOp::Sub; return true;
        case TokKind::StarEq: op = AssignOp::Mul; return true;
        case TokKind::SlashEq: op = AssignOp::Div; return true;
        case TokKind::PercentEq: op = AssignOp::Rem; return true;
        case TokKind::AmpEq: op = AssignOp::BitAnd; return true;
        case TokKind::PipeEq: op = AssignOp::BitOr; return true;
        case TokKind::CaretEq: op = AssignOp::BitXor; return true;
        case TokKind::ShlEq: op = AssignOp::Shl; return true;
        case TokKind::ShrEq: op = AssignOp::Shr; return true;
        default: return false;
    }
}

bool is_open(TokKind k) { return k == TokKind::LParen || k == TokKind::LBracket || k == TokKind::LBrace; }
bool is_close(TokKind k) { return k == TokKind::RParen || k == TokKind::RBracket || k == TokKind::RBrace; }

bool is_item_start(TokKind k) {
    return k == TokKind::KwFn || k == TokKind::KwStruct || k == TokKind::KwEnum || k == TokKind::KwConst ||
           k == TokKind::KwImport || k == TokKind::KwPub || k == TokKind::At;
}

// Ключевые слова, которые начинают инструкцию и не бывают внутри
// выражения: на них восстановление останавливается даже внутри скобок.
bool is_stmt_start(TokKind k) {
    switch (k) {
        case TokKind::KwLet: case TokKind::KwVar: case TokKind::KwIf: case TokKind::KwWhile: case TokKind::KwFor:
        case TokKind::KwParallel: case TokKind::KwMatch: case TokKind::KwReturn: case TokKind::KwBreak:
        case TokKind::KwContinue: case TokKind::KwDefer: case TokKind::KwErrdefer: case TokKind::KwFail: return true;
        default: return false;
    }
}

// Путь типа или модуля в выражении: имя или цепочка `a.b.c`.
bool is_path(Expr* e) {
    if (as<NameExpr>(e)) return true;
    if (auto* f = as<FieldExpr>(e)) return is_path(f->object);
    return false;
}

class Parser {
public:
    Parser(const SourceFile& file, FileId id, Diagnostics& diags, Module& m, uint32_t max_nesting)
        : text_(file.text()), id_(id), diags_(diags), m_(m), toks_(m.tokens), max_nesting_(max_nesting) {}

    void run() {
        nl_.push_back(false);
        while (true) {
            skip_newlines();
            if (peek() == TokKind::Eof) break;
            failed_ = false;
            start_ = idx();
            Item* it = item();
            if (it) m_.items.push_back(it);
            if (failed_ && idx() == start_) bump();  // ничего не взято — шаг вперёд, чтобы не зациклиться
            if (failed_) recover_item();
            else end_of_line(true);
        }
    }

    // Разбор остановлен пределом вложенности (E0002).
    bool stopped() const { return stopped_; }

private:
    std::string_view text_;
    FileId id_;
    Diagnostics& diags_;
    Module& m_;
    const std::vector<Token>& toks_;
    size_t pos_ = 0;           // текущая позиция в toks_ (может указывать на пустоту)
    std::vector<bool> nl_;     // вершина: true — переводы строки незначимы (внутри скобок)
    uint32_t prev_end_ = 0;    // конец последнего взятого значимого токена
    bool failed_ = false;      // в текущей инструкции или объявлении уже есть ошибка
    size_t start_ = 0;         // первый токен текущей инструкции — для восстановления
    uint32_t last_error_ = UINT32_MAX;
    Code last_code_ = Code::E0001;
    uint32_t max_nesting_;
    uint32_t depth_ = 0;       // глубина рекурсии разбора
    bool stopped_ = false;     // предел вложенности: разбор модуля остановлен

    // Уровень вложенности разбора (parser.h, ParseOptions). Сверх предела —
    // E0002 и остановка: дальше модуль не разбирается, остальные сообщения
    // были бы следствием.
    struct Nest {
        Parser& p;
        bool ok;
        explicit Nest(Parser& parser) : p(parser), ok(!parser.stopped_ && parser.depth_ < parser.max_nesting_) {
            if (ok) p.depth_++;
            else p.stop(p.tok_span(p.toks_[p.idx()]));
        }
        ~Nest() {
            if (ok) p.depth_--;
        }
    };

    void stop(Span at) {
        if (stopped_) return;
        failed_ = false;
        error_at(Code::E0002, at, "nesting is too deep: more than " + std::to_string(max_nesting_) + " levels");
        stopped_ = true;
        failed_ = true;
        pos_ = toks_.size() - 1;  // Eof: циклы разбора кончаются
    }

    struct NlScope {
        Parser& p;
        NlScope(Parser& parser, bool skip) : p(parser) { p.nl_.push_back(skip); }
        ~NlScope() { p.nl_.pop_back(); }
    };

    // --- токены

    size_t idx_from(size_t i, bool skip_nl, size_t k) const {
        while (true) {
            TokKind t = toks_[i].kind;
            if (is_trivia(t) || (skip_nl && t == TokKind::Newline)) {
                i++;
                continue;
            }
            if (k == 0 || t == TokKind::Eof) return i;
            k--;
            i++;
        }
    }
    size_t idx(size_t k = 0) const { return idx_from(pos_, nl_.back(), k); }
    TokKind peek(size_t k = 0) const { return toks_[idx(k)].kind; }
    // Вид следующего значимого токена, пропуская и переводы строки.
    TokKind peek_past_newlines() const { return toks_[idx_from(pos_, true, 0)].kind; }
    bool at(TokKind k) const { return peek() == k; }
    std::string_view text_of(const Token& t) const { return text_.substr(t.begin, t.end - t.begin); }

    Token bump() {
        size_t i = idx();
        pos_ = i + 1;
        prev_end_ = toks_[i].end;
        return toks_[i];
    }

    bool eat(TokKind k) {
        if (!at(k)) return false;
        bump();
        return true;
    }

    void skip_newlines() {
        while (true) {
            size_t i = idx();
            if (toks_[i].kind != TokKind::Newline) return;
            pos_ = i + 1;
        }
    }

    uint32_t start() const { return toks_[idx()].begin; }
    Span span_from(uint32_t begin) const { return Span{id_, begin, prev_end_ > begin ? prev_end_ : begin}; }
    Span tok_span(const Token& t) const { return Span{id_, t.begin, t.end}; }

    std::string describe(const Token& t) const {
        switch (t.kind) {
            case TokKind::Ident: return "identifier '" + std::string(text_of(t)) + "'";
            case TokKind::Newline: return "end of line";
            case TokKind::Int: case TokKind::Float: case TokKind::Str: case TokKind::Byte:
                return tok_describe(t.kind) + " " + std::string(text_of(t));
            default: return tok_describe(t.kind);
        }
    }

    // Ошибка «ожидалось …» у следующего токена. Одна на инструкцию; у
    // токена ошибки лексики не сообщаем — лексер уже сообщил.
    void expected(std::string what) {
        if (stopped_) failed_ = true;
        if (failed_) return;
        failed_ = true;
        const Token& t = toks_[idx()];
        if (t.kind == TokKind::Error) return;
        failed_ = false;
        error_at(Code::E0201, tok_span(t), "expected " + what + ", found " + describe(t));
    }

    void error_at(Code code, Span span, std::string msg) {
        if (stopped_) failed_ = true;
        if (failed_) return;
        failed_ = true;
        if (span.begin == last_error_ && code == last_code_) return;  // та же ошибка из внешнего узла
        last_error_ = span.begin;
        last_code_ = code;
        diags_.error(code, span, std::move(msg));
    }

    // Токен i — первый значимый на своей строке.
    bool at_line_start(size_t i) const {
        while (i > 0) {
            TokKind k = toks_[i - 1].kind;
            if (k == TokKind::Newline) return true;
            if (!is_trivia(k)) return false;
            i--;
        }
        return true;
    }

    bool expect(TokKind k) {
        if (eat(k)) return true;
        expected(tok_describe(k));
        return false;
    }

    std::string expect_ident(Span& span) {
        if (at(TokKind::Ident)) {
            Token t = bump();
            span = tok_span(t);
            return std::string(text_of(t));
        }
        expected("identifier");
        span = tok_span(toks_[idx()]);
        return "";
    }

    // --- восстановление

    // Глубина скобок от начала инструкции до текущей позиции.
    int open_depth() const {
        int d = 0;
        for (size_t i = start_; i < pos_ && i < toks_.size(); i++) {
            if (is_open(toks_[i].kind)) d++;
            else if (is_close(toks_[i].kind) && d > 0) d--;
        }
        return d;
    }

    // Пропускает остаток инструкции: до конца строки или `}` на глубине 0,
    // или до строки, которая начинается инструкцией или объявлением.
    void recover_stmt() {
        int d = open_depth();
        bool first = true;
        while (true) {
            size_t i = idx_from(pos_, false, 0);
            TokKind k = toks_[i].kind;
            if (k == TokKind::Eof) return;
            if (d == 0 && (k == TokKind::Newline || k == TokKind::RBrace)) return;
            if (!first && at_line_start(i) && (is_stmt_start(k) || is_item_start(k))) return;
            first = false;
            if (is_open(k)) d++;
            else if (is_close(k) && d > 0) d--;
            pos_ = i + 1;
        }
    }

    // Пропускает остаток объявления: до строки, которая начинается
    // объявлением на глубине 0.
    void recover_item() {
        int d = open_depth();
        while (true) {
            size_t i = idx_from(pos_, false, 0);
            TokKind k = toks_[i].kind;
            if (k == TokKind::Eof) return;
            // Объявление в начале строки — даже внутри незакрытых скобок:
            // внутри тела оно стоять не может.
            if (i > start_ && at_line_start(i) && is_item_start(k)) return;
            if (is_open(k)) d++;
            else if (is_close(k) && d > 0) d--;
            pos_ = i + 1;
        }
    }

    // Конец инструкции или объявления: перевод строки, `}` или конец файла.
    void end_of_line(bool item_level) {
        TokKind k = peek();
        if (k == TokKind::Newline || k == TokKind::Eof || (!item_level && k == TokKind::RBrace)) return;
        const Token& t = toks_[idx()];
        if (k == TokKind::Semicolon) {
            error_at(Code::E0202, tok_span(t), "unexpected ';': statements end at the end of a line");
        } else if (t.kind != TokKind::Error) {
            error_at(Code::E0204, tok_span(t), "expected end of line, found " + describe(t));
        } else {
            failed_ = true;
        }
        if (item_level) recover_item();
        else recover_stmt();
    }

    // --- объявления

    Item* item() {
        uint32_t begin = start();
        std::vector<Attr> attrs;
        while (at(TokKind::At)) {
            Attr a;
            if (!attr(a)) return nullptr;
            attrs.push_back(a);
            skip_newlines();
        }
        bool pub = false;
        Span pub_span;
        if (at(TokKind::KwPub)) {
            pub_span = tok_span(bump());
            pub = true;
        }
        Item* it = nullptr;
        switch (peek()) {
            case TokKind::KwFn: it = fn_item(); break;
            case TokKind::KwExtern: it = extern_item(); break;
            case TokKind::KwStruct: it = struct_item(); break;
            case TokKind::KwEnum: it = enum_item(); break;
            case TokKind::KwConst: it = const_item(); break;
            case TokKind::KwImport:
                if (pub) error_at(Code::E0206, pub_span, "'pub' cannot be applied to 'import'");
                it = import_item();
                break;
            default:
                expected("declaration (fn, struct, enum, const, import or extern)");
                return nullptr;
        }
        if (!it) return nullptr;
        if (!attrs.empty()) {
            if (auto* f = as<FnItem>(it)) f->attrs = std::move(attrs);
            else if (auto* im = as<ImportItem>(it)) im->attrs = std::move(attrs);
            else if (auto* st = as<StructItem>(it)) st->attrs = std::move(attrs);
            else if (auto* et = as<ExternTypeItem>(it)) et->attrs = std::move(attrs);
            else error_at(Code::E0205, attrs[0].span,
                          "attributes are only allowed before a function, a struct, an 'extern type' or an import");
        }
        it->pub = pub;
        it->span = span_from(begin);
        return it;
    }

    // `@имя`, `@имя("строка")`, `@имя(поле)` (Р62: `@count(n)`); false — ошибка.
    bool attr(Attr& a) {
        Token at_tok = bump();
        Span s;
        a.name = expect_ident(s);
        a.span = Span{id_, at_tok.begin, s.end};
        if (failed_) return false;
        if (eat(TokKind::LParen)) {  // `@link("SDL3")`, `@count(n)`, `@slot(4)`
            if (at(TokKind::Ident) || at(TokKind::Int)) {
                a.has_arg = true;
                a.arg_int = at(TokKind::Int);
                a.arg_ident = !a.arg_int;
                a.arg_src = std::string(text_of(bump()));
                a.arg = a.arg_src;
            } else if (at(TokKind::Str)) {
                a.has_arg = true;
                a.arg_src = std::string(text_of(bump()));
                a.arg = string_literal_value(a.arg_src);
            } else {
                expected("a string, a name or a number");
                return false;
            }
            if (!expect(TokKind::RParen)) return false;
            a.span.end = prev_end_;
        }
        return true;
    }

    // `extern fn f(...) T` без тела или `extern type Name` (Р59).
    Item* extern_item() {
        bump();  // extern
        if (at(TokKind::KwFn)) {
            auto* f = static_cast<FnItem*>(fn_item(/*is_extern=*/true));
            return f;
        }
        if (at(TokKind::Ident) && text_of(toks_[idx()]) == "type") {
            bump();
            auto* t = m_.make<ExternTypeItem>();
            t->name = expect_ident(t->name_span);
            return t;
        }
        expected("'fn' or 'type' after 'extern'");
        return nullptr;
    }

    Item* fn_item(bool is_extern = false) {
        auto* f = m_.make<FnItem>();
        f->is_extern = is_extern;
        bump();  // fn
        f->name = expect_ident(f->name_span);
        if (!expect(TokKind::LParen)) return f;
        {
            NlScope nl(*this, true);
            while (!at(TokKind::RParen) && !failed_) {
                f->params.push_back(param());
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RParen);
        }
        if (failed_) return f;
        if (is_extern) {  // без тела: тип результата — до конца строки
            if (!at(TokKind::Newline) && !at(TokKind::Eof)) f->ret = type();
            return f;
        }
        if (!at(TokKind::LBrace)) f->ret = type();
        if (failed_) return f;
        f->body = block();
        return f;
    }

    Param param() {
        Param p;
        uint32_t begin = start();
        p.name = expect_ident(p.name_span);
        expect(TokKind::Colon);
        if (eat(TokKind::Amp)) p.mode = eat(TokKind::KwMut) ? ParamMode::Mut : ParamMode::Ref;
        p.type = type();
        p.span = span_from(begin);
        return p;
    }

    FieldDecl field_decl(bool allow_default) {
        FieldDecl fd;
        uint32_t begin = start();
        while (at(TokKind::At) && !failed_) {  // `@count(n) xs: []T` (Р62)
            Attr a;
            if (!attr(a)) return fd;
            fd.attrs.push_back(a);
        }
        fd.name = expect_ident(fd.name_span);
        expect(TokKind::Colon);
        fd.type = type();
        if (allow_default && eat(TokKind::Eq)) {
            skip_newlines();
            fd.value = expr(false);
        }
        fd.span = span_from(begin);
        return fd;
    }

    // Элементы в фигурных скобках по одному на строке (запятая тоже
    // разделяет): поля структуры, варианты перечисления.
    template <class F>
    void line_list(F each) {
        if (!expect(TokKind::LBrace)) return;
        NlScope nl(*this, false);
        while (true) {
            skip_newlines();
            if (at(TokKind::RBrace) || at(TokKind::Eof) || failed_) break;
            each();
            if (failed_) break;
            if (eat(TokKind::Comma)) continue;
            TokKind k = peek();
            if (k != TokKind::Newline && k != TokKind::RBrace) {
                expected("end of line or '}'");
                break;
            }
        }
        if (!failed_) expect(TokKind::RBrace);
    }

    Item* struct_item() {
        auto* s = m_.make<StructItem>();
        bump();  // struct
        s->name = expect_ident(s->name_span);
        if (failed_) return s;
        line_list([&] { s->fields.push_back(field_decl(true)); });
        return s;
    }

    Item* enum_item() {
        auto* e = m_.make<EnumItem>();
        bump();  // enum
        e->name = expect_ident(e->name_span);
        if (failed_) return e;
        line_list([&] { e->variants.push_back(variant_decl()); });
        return e;
    }

    VariantDecl variant_decl() {
        VariantDecl v;
        uint32_t begin = start();
        v.name = expect_ident(v.name_span);
        if (at(TokKind::LParen)) {
            bump();
            v.form = VariantForm::Tuple;
            NlScope nl(*this, true);
            while (!at(TokKind::RParen) && !failed_) {
                v.types.push_back(type());
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RParen);
        } else if (at(TokKind::LBrace)) {
            bump();
            v.form = VariantForm::Record;
            NlScope nl(*this, true);
            while (!at(TokKind::RBrace) && !failed_) {
                v.fields.push_back(field_decl(false));
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RBrace);
        }
        v.span = span_from(begin);
        return v;
    }

    Item* const_item() {
        auto* c = m_.make<ConstItem>();
        bump();  // const
        c->name = expect_ident(c->name_span);
        expect(TokKind::Colon);
        c->type = type();
        if (expect(TokKind::Eq)) {
            skip_newlines();
            c->value = expr(false);
        }
        return c;
    }

    Item* import_item() {
        auto* im = m_.make<ImportItem>();
        bump();  // import
        im->name = expect_ident(im->name_span);
        return im;
    }

    // --- типы

    Type* type() {
        uint32_t begin = start();
        Nest nest(*this);
        if (!nest.ok) {
            auto* t = m_.make<ErrorType>();
            t->span = span_from(begin);
            return t;
        }
        if (eat(TokKind::Question)) {
            auto* t = m_.make<OptionalType>();
            t->inner = type();
            t->span = span_from(begin);
            return t;
        }
        if (eat(TokKind::Bang)) {
            auto* t = m_.make<ResultType>();
            t->value = type();
            t->span = span_from(begin);
            return t;
        }
        if (at(TokKind::LBracket)) {
            bump();
            if (at(TokKind::Int)) {  // `[N]T` — массив C (Р62)
                auto* t = m_.make<ArrayType>();
                t->len_src = std::string(text_of(toks_[idx()]));
                parse_int_literal(text_of(bump()), t->len);
                expect(TokKind::RBracket);
                t->elem = type();
                t->span = span_from(begin);
                return t;
            }
            auto* t = m_.make<SliceType>();
            expect(TokKind::RBracket);
            t->elem = type();
            t->span = span_from(begin);
            return t;
        }
        if (eat(TokKind::Amp)) {  // `&T` — заимствующее поле `@c` (Р62)
            auto* t = m_.make<RefType>();
            t->inner = type();
            t->span = span_from(begin);
            return t;
        }
        if (eat(TokKind::KwExtern)) {  // `extern fn(&i32, T) R` (Р61)
            auto* t = m_.make<FnType>();
            if (!expect(TokKind::KwFn) || !expect(TokKind::LParen)) {
                t->span = span_from(begin);
                return t;
            }
            {
                NlScope nl(*this, true);
                while (!at(TokKind::RParen) && !failed_) {
                    FnType::Param p;
                    if (eat(TokKind::Amp)) p.mode = eat(TokKind::KwMut) ? ParamMode::Mut : ParamMode::Ref;
                    p.type = type();
                    t->params.push_back(p);
                    if (!eat(TokKind::Comma)) break;
                }
                if (!failed_) expect(TokKind::RParen);
            }
            // Результат — если дальше начинается тип: в списке параметров
            // за типом идут `,` и `)`, в поле и `let` — конец строки и `=`.
            if (!failed_ && (at(TokKind::Ident) || at(TokKind::Question) || at(TokKind::Bang) ||
                             at(TokKind::LBracket) || at(TokKind::KwExtern))) {
                t->ret = type();
            }
            t->span = span_from(begin);
            return t;
        }
        if (!at(TokKind::Ident)) {
            expected("type");
            auto* t = m_.make<ErrorType>();
            t->span = span_from(begin);
            return t;
        }
        auto* t = m_.make<NameType>();
        Span s;
        t->name = expect_ident(s);
        if (eat(TokKind::Dot)) {
            t->module = t->name;
            t->name = expect_ident(s);
        }
        if (at(TokKind::LBracket)) {
            bump();
            NlScope nl(*this, true);
            while (!at(TokKind::RBracket) && !failed_) {
                t->args.push_back(type());
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RBracket);
        }
        t->span = span_from(begin);
        if (at(TokKind::Bang)) {  // `E!T`
            bump();
            auto* r = m_.make<ResultType>();
            r->error = t;
            r->value = type();
            r->span = span_from(begin);
            return r;
        }
        return t;
    }

    // --- инструкции

    Block* block() {
        auto* b = m_.make<Block>();
        uint32_t begin = start();
        Nest nest(*this);
        if (!nest.ok || !expect(TokKind::LBrace)) {
            b->span = span_from(begin);
            return b;
        }
        bool saved_failed = failed_;
        size_t saved_start = start_;
        {
            NlScope nl(*this, false);
            while (true) {
                skip_newlines();
                TokKind k = peek();
                if (k == TokKind::RBrace || k == TokKind::Eof) break;
                // Объявление в начале строки — значит, `}` забыта: блок кончился.
                if (is_item_start(k) && k != TokKind::At && at_line_start(idx())) break;
                failed_ = false;
                start_ = idx();
                Stmt* s = stmt();
                if (s) b->stmts.push_back(s);
                if (failed_) recover_stmt();
                else end_of_line(false);
            }
        }
        failed_ = saved_failed;
        start_ = saved_start;
        expect(TokKind::RBrace);
        b->span = span_from(begin);
        return b;
    }

    Stmt* block_stmt() {
        auto* s = m_.make<BlockStmt>();
        s->block = block();
        s->span = s->block->span;
        return s;
    }

    Stmt* stmt() {
        uint32_t begin = start();
        Nest nest(*this);
        if (!nest.ok) {
            Stmt* e = m_.make<ErrorStmt>();
            e->span = span_from(begin);
            return e;
        }
        Stmt* s = nullptr;
        switch (peek()) {
            case TokKind::KwLet: case TokKind::KwVar: s = let_stmt(); break;
            case TokKind::KwIf: s = if_stmt(); break;
            case TokKind::KwWhile: {
                bump();
                auto* w = m_.make<WhileStmt>();
                w->cond = expr(true);
                if (!failed_) w->body = block();
                s = w;
                break;
            }
            case TokKind::KwFor: s = for_stmt(false); break;
            case TokKind::KwParallel:
                bump();
                if (!at(TokKind::KwFor)) {
                    expected("'for' after 'parallel'");
                    s = m_.make<ErrorStmt>();
                } else {
                    s = for_stmt(true);
                }
                break;
            case TokKind::KwMatch: s = match_stmt(); break;
            case TokKind::KwReturn: {
                bump();
                auto* r = m_.make<ReturnStmt>();
                TokKind k = peek();
                if (k != TokKind::Newline && k != TokKind::RBrace && k != TokKind::Eof) r->value = expr(false);
                s = r;
                break;
            }
            case TokKind::KwBreak: bump(); s = m_.make<BreakStmt>(); break;
            case TokKind::KwContinue: bump(); s = m_.make<ContinueStmt>(); break;
            case TokKind::KwDefer: case TokKind::KwErrdefer: {
                auto* d = m_.make<DeferStmt>();
                d->on_error = bump().kind == TokKind::KwErrdefer;
                d->body = at(TokKind::LBrace) ? block_stmt() : stmt();
                s = d;
                break;
            }
            case TokKind::KwFail: {
                bump();
                auto* f = m_.make<FailStmt>();
                f->value = expr(false);
                s = f;
                break;
            }
            case TokKind::LBrace: s = block_stmt(); break;
            case TokKind::Ident:
                // `lock m as t { … }` (Р70): слово контекстное — за
                // ним имя, а выражение не начинается двумя именами подряд.
                if (text_of(toks_[idx()]) == "lock" && peek(1) == TokKind::Ident) {
                    s = lock_stmt();
                    break;
                }
                // `tasks { … }` (вариант А): выражение не начинается именем и
                // скобкой блока — литерал структуры пишется с именем типа.
                if (text_of(toks_[idx()]) == "tasks" && peek(1) == TokKind::LBrace) {
                    bump();
                    auto* t = m_.make<TasksStmt>();
                    t->body = block();
                    s = t;
                    break;
                }
                s = expr_or_assign();
                break;
            default: s = expr_or_assign(); break;
        }
        s->span = span_from(begin);
        return s;
    }

    Stmt* lock_stmt() {
        bump();  // lock
        auto* l = m_.make<LockStmt>();
        l->mutex = expr(true);
        if (failed_) return l;
        Span as_span;
        std::string as = expect_ident(as_span);
        if (failed_) return l;
        if (as != "as") {
            expected("'as' after the mutex: 'lock m as t { ... }'");
            return l;
        }
        l->var = expect_ident(l->var_span);
        if (!failed_) l->body = block();
        return l;
    }

    Stmt* let_stmt() {
        auto* l = m_.make<LetStmt>();
        l->is_var = bump().kind == TokKind::KwVar;
        l->name = expect_ident(l->name_span);
        if (!failed_ && at(TokKind::LBrace)) {  // разбор `let S { f, g: x } = v` (Р60)
            l->destructure = true;
            bump();
            NlScope nl(*this, true);
            while (!at(TokKind::RBrace) && !failed_) {
                LetField f;
                f.field = expect_ident(f.field_span);
                f.name = f.field;
                f.name_span = f.field_span;
                if (eat(TokKind::Colon)) f.name = expect_ident(f.name_span);
                l->fields.push_back(f);
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RBrace);
            if (failed_) return l;
            if (expect(TokKind::Eq)) {
                skip_newlines();
                l->value = expr(false);
            }
            return l;
        }
        if (eat(TokKind::Colon)) l->type = type();
        if (failed_) return l;
        if (eat(TokKind::Eq)) {
            skip_newlines();
            l->value = expr(false);
        } else if (!l->is_var || !l->type) {
            expected("'=' and an initial value");
        }
        return l;
    }

    // `else if` — рекурсия без stmt(): уровень вложенности считается здесь.
    Stmt* if_stmt() {
        auto* s = m_.make<IfStmt>();
        uint32_t begin = start();
        Nest nest(*this);
        if (!nest.ok) return s;
        bump();  // if
        if (eat(TokKind::KwLet)) {
            s->let_name = expect_ident(s->let_span);
            expect(TokKind::Eq);
            s->let_value = expr(true);
        } else {
            s->cond = expr(true);
        }
        if (failed_) return s;
        s->then = block();
        if (!failed_ && peek_past_newlines() == TokKind::KwElse) {
            skip_newlines();
            bump();  // else
            s->els = at(TokKind::KwIf) ? if_stmt() : block_stmt();
        }
        s->span = span_from(begin);
        return s;
    }

    Stmt* for_stmt(bool parallel) {
        auto* f = m_.make<ForStmt>();
        bump();  // for
        f->parallel = parallel;
        f->var = expect_ident(f->var_span);
        expect(TokKind::KwIn);
        if (failed_) return f;
        if (eat(TokKind::Amp)) {
            f->mode = eat(TokKind::KwMut) ? ForMode::Mut : ForMode::Ref;
            f->iter = expr(true);
        } else {
            f->iter = expr(true);
            if (eat(TokKind::DotDot)) {
                f->mode = ForMode::Range;
                f->hi = expr(true);
            }
        }
        if (!failed_) f->body = block();
        return f;
    }

    Stmt* match_stmt() {
        auto* m = m_.make<MatchStmt>();
        bump();  // match
        m->subject = expr(true);
        if (failed_ || !expect(TokKind::LBrace)) return m;
        bool saved_failed = failed_;
        size_t saved_start = start_;
        {
            NlScope nl(*this, false);
            while (true) {
                skip_newlines();
                TokKind k = peek();
                if (k == TokKind::RBrace || k == TokKind::Eof) break;
                failed_ = false;
                start_ = idx();
                MatchArm arm;
                uint32_t begin = start();
                arm.pat = pattern();
                if (expect(TokKind::FatArrow)) arm.body = at(TokKind::LBrace) ? block_stmt() : stmt();
                arm.span = span_from(begin);
                if (arm.body) m->arms.push_back(arm);
                if (failed_) recover_stmt();
                else end_of_line(false);
            }
        }
        failed_ = saved_failed;
        start_ = saved_start;
        expect(TokKind::RBrace);
        return m;
    }

    Stmt* expr_or_assign() {
        Expr* e = expr(false);
        AssignOp op;
        if (!failed_ && assign_op(peek(), op)) {
            bump();
            skip_newlines();
            auto* a = m_.make<AssignStmt>();
            a->op = op;
            a->target = e;
            a->value = expr(false);
            return a;
        }
        auto* s = m_.make<ExprStmt>();
        s->expr = e;
        return s;
    }

    // --- выражения (ns — без литералов структур: в условиях if, while, for, match)

    Expr* expr(bool ns) { return binary(1, ns); }

    Expr* binary(int min, bool ns) {
        uint32_t begin = start();
        Nest nest(*this);
        if (!nest.ok) {
            auto* e = m_.make<ErrorExpr>();
            e->span = span_from(begin);
            return e;
        }
        Expr* lhs = unary(ns);
        bool lhs_cmp = false;
        while (!failed_) {
            TokKind k = peek();
            int p = prec(k);
            if (p == 0 || p < min) break;
            Token op = bump();
            if (p == 3 && lhs_cmp) {
                error_at(Code::E0203, tok_span(op), "comparison operators cannot be chained; use '&&'");
            }
            skip_newlines();
            Expr* rhs = binary(p + 1, ns);
            auto* b = m_.make<BinaryExpr>();
            b->op = bin_op(k);
            b->lhs = lhs;
            b->rhs = rhs;
            b->op_span = tok_span(op);
            b->span = span_from(begin);
            lhs = b;
            lhs_cmp = p == 3;
        }
        return lhs;
    }

    // Префиксные операторы вложены друг в друга: уровень на каждый.
    Expr* unary(bool ns) {
        uint32_t begin = start();
        TokKind k = peek();
        bool prefix = k == TokKind::Minus || k == TokKind::Bang || k == TokKind::Tilde || k == TokKind::KwTry ||
                      k == TokKind::Amp;
        // `spawn f(…)` (Р70): слово контекстное — за ним имя.
        bool spawn = k == TokKind::Ident && text_of(toks_[idx()]) == "spawn" && peek(1) == TokKind::Ident;
        if (spawn) {
            Nest nest(*this);
            if (!nest.ok) {
                auto* e = m_.make<ErrorExpr>();
                e->span = span_from(begin);
                return e;
            }
            bump();
            auto* sp = m_.make<SpawnExpr>();
            sp->operand = unary(ns);
            sp->span = span_from(begin);
            return sp;
        }
        if (!prefix) return postfix(primary(ns), ns, begin);
        Nest nest(*this);
        if (!nest.ok) {
            auto* e = m_.make<ErrorExpr>();
            e->span = span_from(begin);
            return e;
        }
        if (k == TokKind::Minus || k == TokKind::Bang || k == TokKind::Tilde) {
            bump();
            auto* u = m_.make<UnaryExpr>();
            u->op = k == TokKind::Minus ? UnOp::Neg : k == TokKind::Bang ? UnOp::Not : UnOp::BitNot;
            u->operand = unary(ns);
            u->span = span_from(begin);
            return u;
        }
        if (k == TokKind::KwTry) {
            bump();
            auto* t = m_.make<TryExpr>();
            t->operand = unary(ns);
            t->span = span_from(begin);
            return t;
        }
        if (k == TokKind::Amp) {
            bump();
            auto* r = m_.make<RefExpr>();
            r->mut = eat(TokKind::KwMut);
            r->operand = unary(ns);
            r->span = span_from(begin);
            return r;
        }
        return postfix(primary(ns), ns, begin);
    }

    Expr* postfix(Expr* e, bool ns, uint32_t begin) {
        while (!failed_) {
            TokKind k = peek();
            if (k == TokKind::LParen) {
                bump();
                auto* c = m_.make<CallExpr>();
                c->callee = e;
                {
                    NlScope nl(*this, true);
                    while (!at(TokKind::RParen) && !failed_) {
                        c->args.push_back(expr(false));
                        if (!eat(TokKind::Comma)) break;
                    }
                    if (!failed_) expect(TokKind::RParen);
                }
                e = c;
            } else if (k == TokKind::LBracket) {
                bump();
                NlScope nl(*this, true);
                Expr* lo = at(TokKind::DotDot) ? nullptr : expr(false);
                if (eat(TokKind::DotDot)) {
                    auto* s = m_.make<SliceExpr>();
                    s->object = e;
                    s->lo = lo;
                    if (!at(TokKind::RBracket)) s->hi = expr(false);
                    e = s;
                } else {
                    auto* ix = m_.make<IndexExpr>();
                    ix->object = e;
                    ix->index = lo;
                    e = ix;
                }
                if (!failed_) expect(TokKind::RBracket);
            } else if (k == TokKind::Dot) {
                bump();
                auto* f = m_.make<FieldExpr>();
                f->object = e;
                f->name = expect_ident(f->name_span);
                e = f;
            } else if (k == TokKind::LBrace && !ns && is_path(e)) {
                auto* s = m_.make<StructLitExpr>();
                s->path = e;
                s->fields = field_inits();
                e = s;
            } else {
                break;
            }
            e->span = span_from(begin);
        }
        return e;
    }

    std::vector<FieldInit> field_inits() {
        std::vector<FieldInit> out;
        bump();  // {
        NlScope nl(*this, true);
        while (!at(TokKind::RBrace) && !failed_) {
            FieldInit fi;
            fi.name = expect_ident(fi.name_span);
            if (eat(TokKind::Colon)) fi.value = expr(false);
            out.push_back(fi);
            if (!eat(TokKind::Comma)) break;
        }
        if (!failed_) expect(TokKind::RBrace);
        return out;
    }

    template <class T>
    T* leaf(const Token& t) {
        auto* e = m_.make<T>();
        e->span = tok_span(t);
        return e;
    }

    Expr* primary(bool ns) {
        uint32_t begin = start();
        const Token& t = toks_[idx()];
        switch (t.kind) {
            case TokKind::Int: {
                auto* e = leaf<IntExpr>(bump());
                parse_int_literal(text_of(t), e->value);
                return e;
            }
            case TokKind::Float: {
                auto* e = leaf<FloatExpr>(bump());
                parse_float_literal(text_of(t), e->value);
                return e;
            }
            case TokKind::Str: {
                auto* e = leaf<StrExpr>(bump());
                e->value = string_literal_value(text_of(t));
                return e;
            }
            case TokKind::Byte: {
                auto* e = leaf<ByteExpr>(bump());
                e->value = byte_literal_value(text_of(t));
                return e;
            }
            case TokKind::KwTrue: case TokKind::KwFalse: {
                auto* e = leaf<BoolExpr>(bump());
                e->value = t.kind == TokKind::KwTrue;
                return e;
            }
            case TokKind::KwNone: return leaf<NoneExpr>(bump());
            case TokKind::Ident: {
                auto* e = leaf<NameExpr>(bump());
                e->name = std::string(text_of(t));
                return e;
            }
            case TokKind::LParen: {
                bump();
                auto* p = m_.make<ParenExpr>();
                {
                    NlScope nl(*this, true);
                    p->inner = expr(false);
                    if (!failed_) expect(TokKind::RParen);
                }
                p->span = span_from(begin);
                return p;
            }
            case TokKind::LBracket: {
                bump();
                auto* l = m_.make<ListLitExpr>();
                {
                    NlScope nl(*this, true);
                    while (!at(TokKind::RBracket) && !failed_) {
                        l->items.push_back(expr(false));
                        if (!eat(TokKind::Comma)) break;
                    }
                    if (!failed_) expect(TokKind::RBracket);
                }
                l->span = span_from(begin);
                return l;
            }
            case TokKind::Dot: {
                bump();
                auto* v = m_.make<VariantLitExpr>();
                v->name = expect_ident(v->name_span);
                if (!failed_ && at(TokKind::LParen)) {
                    bump();
                    v->form = VariantForm::Tuple;
                    NlScope nl(*this, true);
                    while (!at(TokKind::RParen) && !failed_) {
                        v->args.push_back(expr(false));
                        if (!eat(TokKind::Comma)) break;
                    }
                    if (!failed_) expect(TokKind::RParen);
                } else if (!failed_ && !ns && at(TokKind::LBrace)) {
                    v->form = VariantForm::Record;
                    v->fields = field_inits();
                }
                v->span = span_from(begin);
                return v;
            }
            default: {
                expected("expression");
                auto* e = m_.make<ErrorExpr>();
                e->span = tok_span(t);
                return e;
            }
        }
    }

    // --- образцы

    Pat* pattern() {
        uint32_t begin = start();
        Nest nest(*this);
        if (!nest.ok) {
            auto* e = m_.make<ErrorPat>();
            e->span = span_from(begin);
            return e;
        }
        Pat* first = pattern_one();
        if (!at(TokKind::Pipe)) return first;
        auto* o = m_.make<OrPat>();
        o->alts.push_back(first);
        while (!failed_ && eat(TokKind::Pipe)) o->alts.push_back(pattern_one());
        o->span = span_from(begin);
        return o;
    }

    Pat* pattern_one() {
        uint32_t begin = start();
        const Token& t = toks_[idx()];
        Pat* p = nullptr;
        switch (t.kind) {
            case TokKind::Ident: {
                std::string name(text_of(t));
                if (name == "_") {
                    bump();
                    p = m_.make<WildPat>();
                    break;
                }
                if ((name == "some" || name == "ok" || name == "err") && peek(1) == TokKind::LParen) {
                    bump();
                    bump();
                    auto* w = m_.make<WrapPat>();
                    w->which = name == "some" ? WrapKind::Some : name == "ok" ? WrapKind::Ok : WrapKind::Err;
                    {
                        NlScope nl(*this, true);
                        w->inner = pattern();
                        if (!failed_) expect(TokKind::RParen);
                    }
                    p = w;
                    break;
                }
                if (peek(1) == TokKind::Dot) {  // `E.V`, `m.E.V`
                    auto* v = m_.make<VariantPat>();
                    v->path.push_back(name);
                    bump();
                    while (eat(TokKind::Dot)) {
                        Span s;
                        std::string n = expect_ident(s);
                        if (at(TokKind::Dot)) {
                            v->path.push_back(n);
                        } else {
                            v->name = n;
                            v->name_span = s;
                        }
                    }
                    variant_pat_rest(v);
                    p = v;
                    break;
                }
                bump();
                auto* b = m_.make<BindPat>();
                b->name = name;
                p = b;
                break;
            }
            case TokKind::KwNone: bump(); p = m_.make<NonePat>(); break;
            case TokKind::Dot: {
                bump();
                auto* v = m_.make<VariantPat>();
                v->name = expect_ident(v->name_span);
                variant_pat_rest(v);
                p = v;
                break;
            }
            case TokKind::Minus: case TokKind::Int: case TokKind::Float: case TokKind::Str: case TokKind::Byte:
            case TokKind::KwTrue: case TokKind::KwFalse: {
                auto* l = m_.make<LitPat>();
                l->negative = eat(TokKind::Minus);
                TokKind k = peek();
                if (l->negative && k != TokKind::Int && k != TokKind::Float) {
                    expected("number after '-' in pattern");
                    l->lit = m_.make<ErrorExpr>();
                } else {
                    l->lit = primary(false);
                }
                p = l;
                break;
            }
            default:
                expected("pattern");
                p = m_.make<ErrorPat>();
                break;
        }
        p->span = span_from(begin);
        return p;
    }

    void variant_pat_rest(VariantPat* v) {
        if (failed_) return;
        if (at(TokKind::LParen)) {
            bump();
            v->form = VariantForm::Tuple;
            NlScope nl(*this, true);
            while (!at(TokKind::RParen) && !failed_) {
                v->args.push_back(pattern());
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RParen);
        } else if (at(TokKind::LBrace)) {
            bump();
            v->form = VariantForm::Record;
            NlScope nl(*this, true);
            while (!at(TokKind::RBrace) && !failed_) {
                FieldPat fp;
                fp.name = expect_ident(fp.name_span);
                if (eat(TokKind::Colon)) fp.pat = pattern();
                v->fields.push_back(fp);
                if (!eat(TokKind::Comma)) break;
            }
            if (!failed_) expect(TokKind::RBrace);
        }
    }
};

}  // namespace

std::unique_ptr<ast::Module> parse_module(const SourceFile& file, FileId id, std::string name, Diagnostics& diags,
                                          const ParseOptions& opt) {
    auto m = std::make_unique<ast::Module>();
    m->file = id;
    m->name = std::move(name);
    m->tokens = lex(file, id, diags);
    Parser p(file, id, diags, *m, opt.max_nesting);
    p.run();
    // Цепочки операторов парсер строит циклом: глубину дерева проверяет
    // обход без рекурсии. Слишком глубокое дерево не отдаётся проходам
    // дальше — они рекурсивны.
    bool too_deep = p.stopped();
    if (!too_deep) {
        if (const ast::Node* n = deeper_than(*m, opt.max_nesting)) {
            diags.error(Code::E0002, n->span,
                        "nesting is too deep: more than " + std::to_string(opt.max_nesting) + " levels");
            too_deep = true;
        }
    }
    if (too_deep) m->items.clear();
    return m;
}

}  // namespace dp
