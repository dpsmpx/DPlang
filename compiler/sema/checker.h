// Проверка тел функций, констант и значений полей по умолчанию: имена,
// типы, поток (определённая инициализация, возврат), ошибки, ссылки
// (docs/spec/core.md, разделы 3–9). Внутренний заголовок sema.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "sema/resolve.h"
#include "sema/sema.h"

namespace dp::sema {

// Бюджет вычисления констант одного модуля ([decl.const]): шаг стоит 1
// плюс байты строк, которые он создаёт. Без него `const Ci: str = Cj + Cj`
// удваивает память на строку исходника (docs/policy/SECURITY.md, раздел 4).
constexpr uint64_t kConstEvalBudget = uint64_t{1} << 24;

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
    // Константы раньше зависящих от них — обходом с явным стеком (check.cpp).
    void const_chain(ConstDef& root);
    std::vector<ConstDef*> const_deps(const ConstDef& c);
    // Аргументы вызова с ошибкой — без сообщений о пометках `&`.
    void infer_loose(const std::vector<ast::Expr*>& args);
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
        bool parallel = false;
        bool tasks = false;  // граница блока задач: break и continue за неё не выходят
    };
    std::vector<Loop> loops_;
    int defer_depth_ = 0;
    int parallel_depth_ = 0;  // тела `parallel for` вокруг: из них не выходят (Р69)

    // --- общие
    const Type* err() { return tt_.error(); }
    const Type* prim(TK k) { return tt_.prim(k); }
    std::string tn(const Type* t) { return type_name(t, &m_); }
    Diagnostic& error(Code code, Span span, const std::string& msg) { return diags_.error(code, span, msg); }

    Local* find_local(const std::string& name) const;
    Local* declare(const std::string& name, Span span, const Type* type, bool mutable_, bool is_ref, bool init);
    bool declaring_params_ = false;  // параметры: заимствующий тип можно (Р62)
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
    void lock_stmt(ast::LockStmt* s);
    void tasks_stmt(ast::TasksStmt* s);
    // Мьютексы, захваченные блоками `lock` вокруг (О13): их корни в теле
    // блока не называют — данные только через переменную блока.
    std::vector<const Local*> locked_;
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
    const Type* ptr_call(ast::CallExpr* c, const Local& callee);
    void check_args(ast::CallExpr* c, const std::vector<ParamDef>& params);
    // Функция как значение: только `@export` — указатель на функцию C (Р61).
    const Type* fn_value(const FnDef& f, Span span);
    const Type* builtin_call(ast::CallExpr* c, uint32_t b, const Type* hint);
    const Type* method_call(ast::CallExpr* c, ast::FieldExpr* f);
    const Type* convert_call(ast::CallExpr* c, const Type* target, bool wrap);
    const Type* atomic_method(ast::CallExpr* c, ast::FieldExpr* f, const Type* t, ast::Method m, bool known);
    const Type* channel_method(ast::CallExpr* c, ast::FieldExpr* f, const Type* t, ast::Method m, bool known);
    const Type* variant_value(const EnumDef* en, uint32_t index, ast::VariantForm form, std::vector<ast::Expr*>* args,
                              std::vector<ast::FieldInit>* fields, Span span);
    const Type* struct_lit(ast::StructLitExpr* s);
    const Type* variant_lit(ast::VariantLitExpr* v, const Type* hint);
    const Type* list_lit(ast::ListLitExpr* l, const Type* hint);
    const Type* index_expr(ast::IndexExpr* i);
    const Type* slice_expr(ast::SliceExpr* s);
    const Type* try_expr(ast::TryExpr* t);
    const Type* spawn_expr(ast::SpawnExpr* sp);
    void field_inits(std::vector<ast::FieldInit>& inits, const std::vector<FieldDef>& fields, bool defaults_ok, Span span,
                     const std::string& what, bool c_zero = false);
    const EnumDef* enum_for_variant(const std::string& name, const Type* hint, Span span);
    bool ordered(const Type* t);
    bool equatable(const Type* t);
    bool type_prop(const Type* root, bool ord);
    std::map<const Type*, bool> prop_cache_[2];  // equatable, ordered

    // места: изменяемость и корень (Р44)
    bool is_place(ast::Expr* e);
    // Изменяемое место; иначе — сообщение у e, why — действие («assign to»).
    bool mutable_place(ast::Expr* e, const char* why);
    Local* root_local(ast::Expr* e);
    void ref_args(ast::CallExpr* c, const std::vector<ParamDef>& params);

    // --- исключительный доступ (Р66; access.cpp)
    // Обход `for x in &c` в теле которого идёт проверка.
    struct AccessLoop {
        const Local* root = nullptr;  // корень коллекции; nullptr — диапазон
        const Local* var = nullptr;   // переменная цикла
        ast::Expr* iter = nullptr;
        bool mut = false;             // `for … in &mut`
        bool parallel = false;
        bool range = false;  // `for i in a..b`: i в итерациях разная — `out[i]` у каждой свой
        uint32_t first_inner = 0;  // переменные с номером не меньше — объявлены в теле
        TK coll = TK::Error;       // List, Map, Slice; Error — диапазон или срез str
        std::vector<const Local*> written;  // `parallel for`: внешние переменные, которые тело пишет как out[i]
    };
    std::vector<AccessLoop> access_loops_;
    // Переменная обхода — выражение его коллекции: `for y in &x.items`.
    std::map<const Local*, ast::Expr*> loop_iters_;
    // Чтение коллекции `&mut`-обхода: проверка — в конце обхода, когда место
    // продлено полями и индексами (`w.seed` — не обходимое `w.bodies`).
    struct LoopRead {
        ast::Expr* place = nullptr;
        size_t loop = 0;  // номер в access_loops_
        bool par = false;  // чтение внешней переменной в теле `parallel for` (Р69)
    };
    std::vector<LoopRead> loop_reads_;
    // Запись в место: присваивание, изменяющий метод, `&mut` (новый ключ
    // `m[k] = v` — запись в m). why — действие для сообщения («assign to»);
    // loops = false — только правило `parallel for` (обход сверяет loop_begin).
    void note_write(ast::Expr* place, const char* why, bool loops = true);
    void note_use(ast::NameExpr* n);
    // `len(x)` в теле `parallel for` — не чтение элементов x (Р69).
    void note_len(ast::Expr* e);
    // Выход из тела `parallel for` (Р69) или блока задач: break, return, fail, try.
    bool parallel_exit(Span span, const char* what);

    // --- задачи (Р70; access.cpp)
public:
    // Цикл блока задач вокруг `spawn`: переменная (nullptr — while) и
    // диапазон ли он — у диапазона `out[i]` в итерациях разные, у обхода
    // разные только сами элементы.
    struct TaskLoop {
        const Local* var = nullptr;
        bool range = false;
    };
    // Заимствование `&x` или `&mut x` задачей блока: живёт до конца блока.
    struct TaskBorrow {
        ast::Expr* place = nullptr;
        bool mut = false;
        const ast::SpawnExpr* spawn = nullptr;
        std::vector<TaskLoop> loops;
    };
    // Доступ кода блока (вне задач) к месту: читает или меняет.
    struct TaskAccess {
        ast::Expr* place = nullptr;
        bool write = false;
        bool len = false;  // `len(x)`: читает только длину
    };
    struct TasksScope {
        uint32_t first_inner = 0;  // переменные блока — с номером не меньше
        std::vector<TaskLoop> loops;
        std::vector<TaskBorrow> borrows;
        std::vector<TaskAccess> accesses;
    };

private:
    std::vector<TasksScope> tasks_;
    void task_loop_begin(const ast::ForStmt* f);  // f == nullptr — while
    void task_loop_end();
    void task_spawn(ast::SpawnExpr* sp, ast::CallExpr* c, const std::vector<size_t>& marks);
    void tasks_end();
    void note_extend(ast::Expr* object, ast::Expr* place);
    void loop_begin(ast::ForStmt* s);
    void loop_end();
    // У переменной обхода Map и среза `&mut` не бывает (сообщение E0412).
    bool read_only_loop(const Local* l) const;
    // Два места в одном вызове пересекаются: одно — начало другого или
    // индексы вычислены (E0502).
    bool overlap(ast::Expr* a, ast::Expr* b) const;
    // Вид левее действия, которое меняет его место (E0502): инструкция s.
    void views(ast::Stmt* s);
    void view_check(ast::Expr* e, const std::vector<ast::Expr*>& right);
    void view_operands(const std::vector<ast::Expr*>& ops, const std::vector<ast::Expr*>& right, bool keep);
    // true — ошибка выдана.
    bool view_at(ast::Expr* place, ast::Expr* at, const std::vector<ast::Expr*>& right);
    // Места, которые меняет вычисление e: `&mut` и получатели изменяющих
    // методов; top — без `&mut` самого e (его сверяет E0502 вызова).
    void changes(ast::Expr* e, std::vector<ast::Expr*>& out, bool top = false) const;

    // --- образцы и match (check_pat.cpp)
    void pat(ast::Pat* p, const Type* subject);
    void exhaustive(ast::MatchStmt* m, const Type* subject);

    // --- константы (check_expr.cpp)
    bool const_eval(ast::Expr* e, const Type* want, ConstValue& out);
    // Списывает шаги с бюджета; сверх него — одно E0421 на модуль и false.
    bool const_spend(ast::Expr* e, uint64_t steps);
    uint64_t const_work_ = 0;
    bool const_over_ = false;
};

// Вывод типа ошибки непубличных функций `!T` (Р41) — до проверки тел.
void infer_errors(Program& prog, ModuleInfo& m, Diagnostics& diags);

// Линейные значения (Р57, Р60): закрыты на всех путях, не используются
// после закрытия — после проверки тел, по типизированному дереву.
void check_linear(Program& prog, ModuleInfo& m, Diagnostics& diags);

// Тело `parallel for` без `os` и C по графу вызовов (Р69) — после проверки
// всех модулей.
void check_parallel(Program& prog, Diagnostics& diags);

}  // namespace dp::sema
