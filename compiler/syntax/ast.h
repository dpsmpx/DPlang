// AST модуля (docs/spec/core.md, раздел 2; docs/compiler/README.md).
//
// Дерево без потерь — это токены файла (вместе с пустотами) и AST с
// участками: каждый узел знает свой участок исходника, а печать токенов
// по порядку восстанавливает файл байт в байт. Узлы живут в арене модуля;
// вид узла — поле kind, приведение — as<T>() по виду (RTTI нет, Р34).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "support/source.h"
#include "syntax/lexer.h"

namespace dp::ast {

struct Node {
    Span span;
    virtual ~Node() = default;
};

// --- типы

enum class TypeKind : uint8_t { Name, Slice, Optional, Result, Error };

struct Type : Node {
    TypeKind kind;
    explicit Type(TypeKind k) : kind(k) {}
};

// `T`, `m.T`, `List[T]`.
struct NameType : Type {
    static constexpr TypeKind Kind = TypeKind::Name;
    NameType() : Type(Kind) {}
    std::string module;  // пусто — без модуля
    std::string name;
    std::vector<Type*> args;
};

struct SliceType : Type {  // `[]T`
    static constexpr TypeKind Kind = TypeKind::Slice;
    SliceType() : Type(Kind) {}
    Type* elem = nullptr;
};

struct OptionalType : Type {  // `?T`
    static constexpr TypeKind Kind = TypeKind::Optional;
    OptionalType() : Type(Kind) {}
    Type* inner = nullptr;
};

struct ResultType : Type {  // `!T` или `E!T`
    static constexpr TypeKind Kind = TypeKind::Result;
    ResultType() : Type(Kind) {}
    Type* error = nullptr;  // nullptr — `!T`, тип ошибки выводится (Р41)
    Type* value = nullptr;
};

struct ErrorType : Type {
    static constexpr TypeKind Kind = TypeKind::Error;
    ErrorType() : Type(Kind) {}
};

// --- выражения

enum class ExprKind : uint8_t {
    Int, Float, Str, Byte, Bool, None, Name, Field, Index, Slice, Call, Unary, Binary, Try, Paren, Ref,
    StructLit, VariantLit, ListLit, Error
};

struct Expr : Node {
    ExprKind kind;
    explicit Expr(ExprKind k) : kind(k) {}
};

struct IntExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Int;
    IntExpr() : Expr(Kind) {}
    uint64_t value = 0;
};

struct FloatExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Float;
    FloatExpr() : Expr(Kind) {}
    double value = 0;
};

struct StrExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Str;
    StrExpr() : Expr(Kind) {}
    std::string value;
};

struct ByteExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Byte;
    ByteExpr() : Expr(Kind) {}
    uint8_t value = 0;
};

struct BoolExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Bool;
    BoolExpr() : Expr(Kind) {}
    bool value = false;
};

struct NoneExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::None;
    NoneExpr() : Expr(Kind) {}
};

struct NameExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Name;
    NameExpr() : Expr(Kind) {}
    std::string name;
};

struct FieldExpr : Expr {  // `a.b`
    static constexpr ExprKind Kind = ExprKind::Field;
    FieldExpr() : Expr(Kind) {}
    Expr* object = nullptr;
    std::string name;
    Span name_span;
};

struct IndexExpr : Expr {  // `a[i]`
    static constexpr ExprKind Kind = ExprKind::Index;
    IndexExpr() : Expr(Kind) {}
    Expr* object = nullptr;
    Expr* index = nullptr;
};

struct SliceExpr : Expr {  // `a[lo..hi]`, концы необязательны
    static constexpr ExprKind Kind = ExprKind::Slice;
    SliceExpr() : Expr(Kind) {}
    Expr* object = nullptr;
    Expr* lo = nullptr;
    Expr* hi = nullptr;
};

struct CallExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Call;
    CallExpr() : Expr(Kind) {}
    Expr* callee = nullptr;
    std::vector<Expr*> args;
};

enum class UnOp : uint8_t { Neg, Not, BitNot };

struct UnaryExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Unary;
    UnaryExpr() : Expr(Kind) {}
    UnOp op = UnOp::Neg;
    Expr* operand = nullptr;
};

enum class BinOp : uint8_t {
    Or, And, Eq, Ne, Lt, Le, Gt, Ge, Coalesce, Add, Sub, BitOr, BitXor, AddWrap, SubWrap, Mul, Div, Rem, Shl,
    Shr, BitAnd, MulWrap
};

struct BinaryExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Binary;
    BinaryExpr() : Expr(Kind) {}
    BinOp op = BinOp::Add;
    Expr* lhs = nullptr;
    Expr* rhs = nullptr;
    Span op_span;
};

struct TryExpr : Expr {  // `try e`
    static constexpr ExprKind Kind = ExprKind::Try;
    TryExpr() : Expr(Kind) {}
    Expr* operand = nullptr;
};

struct ParenExpr : Expr {  // `(e)` — смысла не меняет, хранится для печати
    static constexpr ExprKind Kind = ExprKind::Paren;
    ParenExpr() : Expr(Kind) {}
    Expr* inner = nullptr;
};

struct RefExpr : Expr {  // пометка аргумента `&x`, `&mut x`
    static constexpr ExprKind Kind = ExprKind::Ref;
    RefExpr() : Expr(Kind) {}
    bool mut = false;
    Expr* operand = nullptr;
};

struct FieldInit {
    std::string name;
    Span name_span;
    Expr* value = nullptr;  // nullptr — краткая запись `name` вместо `name: name`
};

// `S { ... }`, `m.S { ... }`, `E.V { ... }` — что именно, решает проверка имён.
struct StructLitExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::StructLit;
    StructLitExpr() : Expr(Kind) {}
    Expr* path = nullptr;  // NameExpr или цепочка FieldExpr
    std::vector<FieldInit> fields;
};

enum class VariantForm : uint8_t { Unit, Tuple, Record };

// Краткая запись варианта: `.V`, `.V(a, b)`, `.V { f: a }`.
struct VariantLitExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::VariantLit;
    VariantLitExpr() : Expr(Kind) {}
    std::string name;
    Span name_span;
    VariantForm form = VariantForm::Unit;
    std::vector<Expr*> args;
    std::vector<FieldInit> fields;
};

struct ListLitExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::ListLit;
    ListLitExpr() : Expr(Kind) {}
    std::vector<Expr*> items;
};

struct ErrorExpr : Expr {
    static constexpr ExprKind Kind = ExprKind::Error;
    ErrorExpr() : Expr(Kind) {}
};

// --- образцы match

enum class PatKind : uint8_t { Wild, Lit, Bind, None, Wrap, Variant, Or, Error };

struct Pat : Node {
    PatKind kind;
    explicit Pat(PatKind k) : kind(k) {}
};

struct WildPat : Pat {
    static constexpr PatKind Kind = PatKind::Wild;
    WildPat() : Pat(Kind) {}
};

struct LitPat : Pat {  // литерал; у целого может быть минус
    static constexpr PatKind Kind = PatKind::Lit;
    LitPat() : Pat(Kind) {}
    Expr* lit = nullptr;
    bool negative = false;
};

struct BindPat : Pat {
    static constexpr PatKind Kind = PatKind::Bind;
    BindPat() : Pat(Kind) {}
    std::string name;
};

struct NonePat : Pat {
    static constexpr PatKind Kind = PatKind::None;
    NonePat() : Pat(Kind) {}
};

enum class WrapKind : uint8_t { Some, Ok, Err };

struct WrapPat : Pat {  // `some(p)`, `ok(p)`, `err(p)`
    static constexpr PatKind Kind = PatKind::Wrap;
    WrapPat() : Pat(Kind) {}
    WrapKind which = WrapKind::Some;
    Pat* inner = nullptr;
};

struct FieldPat {
    std::string name;
    Span name_span;
    Pat* pat = nullptr;  // nullptr — краткая запись: связать поле с именем поля
};

// `.V`, `.V(p, q)`, `.V { f, g: p }`; с типом — `E.V`, `m.E.V`.
struct VariantPat : Pat {
    static constexpr PatKind Kind = PatKind::Variant;
    VariantPat() : Pat(Kind) {}
    std::vector<std::string> path;  // пусто — краткая запись
    std::string name;
    Span name_span;
    VariantForm form = VariantForm::Unit;
    std::vector<Pat*> args;
    std::vector<FieldPat> fields;
};

struct OrPat : Pat {
    static constexpr PatKind Kind = PatKind::Or;
    OrPat() : Pat(Kind) {}
    std::vector<Pat*> alts;
};

struct ErrorPat : Pat {
    static constexpr PatKind Kind = PatKind::Error;
    ErrorPat() : Pat(Kind) {}
};

// --- инструкции

enum class StmtKind : uint8_t {
    Let, Assign, Expr, If, While, For, Match, Return, Break, Continue, Defer, Fail, Block, Error
};

struct Stmt : Node {
    StmtKind kind;
    explicit Stmt(StmtKind k) : kind(k) {}
};

struct Block : Node {
    std::vector<Stmt*> stmts;
};

struct LetStmt : Stmt {  // `let x = e`, `var x: T = e`
    static constexpr StmtKind Kind = StmtKind::Let;
    LetStmt() : Stmt(Kind) {}
    bool is_var = false;
    std::string name;
    Span name_span;
    Type* type = nullptr;
    Expr* value = nullptr;
};

enum class AssignOp : uint8_t { Set, Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr };

struct AssignStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Assign;
    AssignStmt() : Stmt(Kind) {}
    AssignOp op = AssignOp::Set;
    Expr* target = nullptr;
    Expr* value = nullptr;
};

struct ExprStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Expr;
    ExprStmt() : Stmt(Kind) {}
    Expr* expr = nullptr;
};

struct IfStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::If;
    IfStmt() : Stmt(Kind) {}
    Expr* cond = nullptr;  // nullptr — `if let`
    std::string let_name;  // `if let v = e`
    Span let_span;
    Expr* let_value = nullptr;
    Block* then = nullptr;
    Stmt* els = nullptr;  // BlockStmt, IfStmt или nullptr
};

struct WhileStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::While;
    WhileStmt() : Stmt(Kind) {}
    Expr* cond = nullptr;
    Block* body = nullptr;
};

enum class ForMode : uint8_t { Value, Ref, Mut, Range };

struct ForStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::For;
    ForStmt() : Stmt(Kind) {}
    bool parallel = false;
    std::string var;  // `_` — без переменной
    Span var_span;
    ForMode mode = ForMode::Value;
    Expr* iter = nullptr;  // коллекция или начало диапазона
    Expr* hi = nullptr;    // конец диапазона
    Block* body = nullptr;
};

struct MatchArm {
    Pat* pat = nullptr;
    Stmt* body = nullptr;  // BlockStmt или простая инструкция
    Span span;
};

struct MatchStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Match;
    MatchStmt() : Stmt(Kind) {}
    Expr* subject = nullptr;
    std::vector<MatchArm> arms;
};

struct ReturnStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Return;
    ReturnStmt() : Stmt(Kind) {}
    Expr* value = nullptr;
};

struct BreakStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Break;
    BreakStmt() : Stmt(Kind) {}
};

struct ContinueStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Continue;
    ContinueStmt() : Stmt(Kind) {}
};

struct DeferStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Defer;
    DeferStmt() : Stmt(Kind) {}
    Stmt* body = nullptr;  // BlockStmt или простая инструкция
};

struct FailStmt : Stmt {  // `fail e`
    static constexpr StmtKind Kind = StmtKind::Fail;
    FailStmt() : Stmt(Kind) {}
    Expr* value = nullptr;
};

struct BlockStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Block;
    BlockStmt() : Stmt(Kind) {}
    Block* block = nullptr;
};

struct ErrorStmt : Stmt {
    static constexpr StmtKind Kind = StmtKind::Error;
    ErrorStmt() : Stmt(Kind) {}
};

// --- объявления

enum class ItemKind : uint8_t { Fn, Struct, Enum, Const, Import, Error };

struct Item : Node {
    ItemKind kind;
    bool pub = false;
    std::string name;
    Span name_span;
    explicit Item(ItemKind k) : kind(k) {}
};

struct Attr {
    std::string name;
    Span span;
};

enum class ParamMode : uint8_t { Value, Ref, Mut };

struct Param {
    std::string name;
    Span name_span;
    ParamMode mode = ParamMode::Value;
    Type* type = nullptr;
    Span span;
};

struct FnItem : Item {
    static constexpr ItemKind Kind = ItemKind::Fn;
    FnItem() : Item(Kind) {}
    std::vector<Attr> attrs;
    std::vector<Param> params;
    Type* ret = nullptr;  // nullptr — без результата
    Block* body = nullptr;
};

struct FieldDecl {
    std::string name;
    Span name_span;
    Type* type = nullptr;
    Expr* value = nullptr;  // значение по умолчанию
    Span span;
};

struct StructItem : Item {
    static constexpr ItemKind Kind = ItemKind::Struct;
    StructItem() : Item(Kind) {}
    std::vector<FieldDecl> fields;
};

struct VariantDecl {
    std::string name;
    Span name_span;
    VariantForm form = VariantForm::Unit;
    std::vector<Type*> types;       // Tuple
    std::vector<FieldDecl> fields;  // Record
    Span span;
};

struct EnumItem : Item {
    static constexpr ItemKind Kind = ItemKind::Enum;
    EnumItem() : Item(Kind) {}
    std::vector<VariantDecl> variants;
};

struct ConstItem : Item {
    static constexpr ItemKind Kind = ItemKind::Const;
    ConstItem() : Item(Kind) {}
    Type* type = nullptr;
    Expr* value = nullptr;
};

struct ImportItem : Item {  // name — имя модуля
    static constexpr ItemKind Kind = ItemKind::Import;
    ImportItem() : Item(Kind) {}
};

struct ErrorItem : Item {
    static constexpr ItemKind Kind = ItemKind::Error;
    ErrorItem() : Item(Kind) {}
};

// Приведение по виду: nullptr, если вид другой.
template <class T, class B>
T* as(B* node) {
    return node && node->kind == T::Kind ? static_cast<T*>(node) : nullptr;
}

// Разобранный модуль: токены (дерево без потерь вместе с участками
// узлов), объявления и арена, которая владеет узлами.
struct Module {
    FileId file = 0;
    std::string name;
    std::vector<Token> tokens;
    std::vector<Item*> items;
    std::vector<std::unique_ptr<Node>> arena;

    template <class T>
    T* make() {
        auto p = std::make_unique<T>();
        T* raw = p.get();
        arena.push_back(std::move(p));
        return raw;
    }
};

}  // namespace dp::ast
