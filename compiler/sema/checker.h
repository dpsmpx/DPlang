// Проверка тел функций, констант и значений полей по умолчанию: имена,
// типы, поток (определённая инициализация, возврат), ошибки, ссылки
// (docs/spec/core.md, разделы 3–9). Внутренний заголовок sema.
#pragma once

#include <vector>

#include "sema/resolve.h"
#include "sema/sema.h"

namespace dp::sema {

class Checker {
public:
    Checker(Program& prog, ModuleInfo& m, Diagnostics& diags);
    void run();

    // Значение константы (вычисляет при первом обращении; цикл — ошибка).
    const ConstValue& const_value(ConstDef& c);

private:
    friend struct ScopeGuard;
    Program& prog_;
    ModuleInfo& m_;
    Diagnostics& diags_;
    Resolver r_;
    TypeTable& tt_;

    // --- области и переменные
    std::vector<std::vector<Local*>> scopes_;
    // Корни Map, которые обходит `for k in &m`: менять их в теле нельзя (Р54).
    std::vector<Local*> frozen_;
    bool frozen(const Local* l) const;
    uint32_t next_local_ = 0;
    int or_depth_ = 0;

    // --- функция и поток
    FnDef* fn_ = nullptr;
    struct Flow {
        std::vector<char> init;  // по номеру переменной: значение точно есть
        bool live = true;        // место достижимо
    };
    Flow flow_;
    struct Loop {
        std::vector<Flow> breaks;
    };
    std::vector<Loop> loops_;
    int defer_depth_ = 0;

    // --- общие
    const Type* err() { return tt_.error(); }
    const Type* prim(TK k) { return tt_.prim(k); }
    std::string tn(const Type* t) { return type_name(t, &m_); }
    Diagnostic& error(Code code, Span span, const std::string& msg) { return diags_.error(code, span, msg); }

    Local* find_local(const std::string& name) const;
    Local* declare(const std::string& name, Span span, const Type* type, bool mutable_, bool is_ref, bool init);
    void push_scope() { scopes_.emplace_back(); }
    void pop_scope() { scopes_.pop_back(); }

    // --- объявления (check.cpp)
    void fn(FnDef& f);
    void struct_defaults(StructDef& s);

    // --- инструкции и поток (check.cpp)
    void block(ast::Block* b);
    void stmt(ast::Stmt* s);
    void let_stmt(ast::LetStmt* l);
    void destructure(ast::LetStmt* l);
    void assign_stmt(ast::AssignStmt* a);
    void expr_stmt(ast::ExprStmt* s);
    void if_stmt(ast::IfStmt* s);
    void while_stmt(ast::WhileStmt* s);
    void for_stmt(ast::ForStmt* s);
    void match_stmt(ast::MatchStmt* s);
    void return_stmt(ast::ReturnStmt* s);
    void fail_stmt(ast::FailStmt* s);
    void loop_jump(ast::Stmt* s, bool is_break);
    Flow merge(const Flow& a, const Flow& b);

    // --- выражения (check_expr.cpp)
public:
    // Тип выражения e; hint — ожидаемый тип для литералов, `none`, `[]`, `.V`.
    const Type* infer(ast::Expr* e, const Type* hint);
    // Проверяет, что e подходит к want (с неявным преобразованием); возвращает want.
    const Type* check(ast::Expr* e, const Type* want);

private:
    void coerce(ast::Expr* e, const Type* want);
    bool convertible(const Type* from, const Type* to);
    bool literal_like(const ast::Expr* e) const;
    const Type* name_expr(ast::NameExpr* n);
    const Type* field_expr(ast::FieldExpr* f, const Type* hint);
    void resolve_name(ast::NameExpr* n);
    void resolve_field(ast::FieldExpr* f);
    const Type* int_lit(ast::IntExpr* e, const Type* hint, bool negative, Span span);
    const Type* unary(ast::UnaryExpr* u, const Type* hint);
    const Type* binary(ast::BinaryExpr* b, const Type* hint);
    const Type* common(ast::Expr* lhs, ast::Expr* rhs, const Type* hint, Span op_span, const char* op);
    const Type* call(ast::CallExpr* c, const Type* hint);
    const Type* user_call(ast::CallExpr* c, const FnDef& f);
    const Type* builtin_call(ast::CallExpr* c, uint32_t b, const Type* hint);
    const Type* method_call(ast::CallExpr* c, ast::FieldExpr* f);
    const Type* convert_call(ast::CallExpr* c, const Type* target, bool wrap);
    const Type* variant_value(const EnumDef* en, uint32_t index, ast::VariantForm form, std::vector<ast::Expr*>* args,
                              std::vector<ast::FieldInit>* fields, Span span);
    const Type* struct_lit(ast::StructLitExpr* s);
    const Type* variant_lit(ast::VariantLitExpr* v, const Type* hint);
    const Type* list_lit(ast::ListLitExpr* l, const Type* hint);
    const Type* index_expr(ast::IndexExpr* i);
    const Type* slice_expr(ast::SliceExpr* s);
    const Type* try_expr(ast::TryExpr* t);
    void field_inits(std::vector<ast::FieldInit>& inits, const std::vector<FieldDef>& fields, bool defaults_ok,
                     Span span, const std::string& what);
    const EnumDef* enum_for_variant(const std::string& name, const Type* hint, Span span);
    bool ordered(const Type* t);
    bool equatable(const Type* t);

    // места: изменяемость и корень (Р44)
    bool is_place(ast::Expr* e);
    // Изменяемое место; иначе — сообщение у e, why — действие («assign to»).
    bool mutable_place(ast::Expr* e, const char* why);
    Local* root_local(ast::Expr* e);
    void ref_args(ast::CallExpr* c, const std::vector<ParamDef>& params);

    // --- образцы и match (check_pat.cpp)
    void pat(ast::Pat* p, const Type* subject);
    void exhaustive(ast::MatchStmt* m, const Type* subject);

    // --- константы (check_expr.cpp)
    bool const_eval(ast::Expr* e, const Type* want, ConstValue& out);
};

// Вывод типа ошибки непубличных функций `!T` (Р41) — до проверки тел.
void infer_errors(Program& prog, ModuleInfo& m, Diagnostics& diags);

// Линейные значения (Р57, Р60): закрыты на всех путях, не используются
// после закрытия — после проверки тел, по типизированному дереву.
void check_linear(Program& prog, ModuleInfo& m, Diagnostics& diags);

}  // namespace dp::sema
