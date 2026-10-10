// Core IR (docs/ir/README.md; docs/design/ARCHITECTURE.md, раздел 1).
//
// Маленькое явное представление без неопределённого поведения: функции из
// базовых блоков, инструкции над местами и операндами, терминаторы.
// Проверки (переполнение, границы, деление на ноль) — часть смысла
// операций и проекций; нарушение — ловушка, детерминированная остановка
// (Р36). Память значений (Р45) явна: copy копирует значение целиком, move
// забирает временное, drop освобождает, присваивание освобождает старое
// значение места.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sema/defs.h"
#include "support/source.h"

namespace dp::ir {

using sema::Type;

// --- переменные функции

enum class LocalKind : uint8_t {
    Param,     // параметр-значение: принадлежит функции
    ParamRef,  // параметр &T: место — значение вызывающего, только чтение
    ParamMut,  // параметр &mut T
    Var,       // переменная программы
    Temp,      // временное: значение выражения, из него можно забрать (move)
};

struct Local {
    std::string name;  // пусто — без имени в исходнике
    const Type* type = nullptr;
    LocalKind kind = LocalKind::Temp;
};

// --- места

enum class ProjKind : uint8_t {
    Field,        // поле структуры: index — номер поля
    Index,        // элемент List или среза, байт str: index — переменная i64 с номером; вне границ — ловушка
    Payload,      // данные варианта index; sub — номер значения (позиция или поле)
    Unwrap,       // значение ?T (есть — проверено раньше)
    ResultValue,  // значение E!T при успехе
    ResultError,  // ошибка E!T
};

struct Proj {
    ProjKind kind = ProjKind::Field;
    uint32_t index = 0;
    uint32_t sub = 0;
};

// Место — переменная и путь в ней. У параметра-ссылки место — значение,
// на которое она указывает.
struct Place {
    uint32_t local = 0;
    std::vector<Proj> proj;
};

// --- операнды

struct Const {
    enum class Kind : uint8_t { Int, Float, Bool, Str } kind = Kind::Int;
    uint64_t i = 0;  // целое: биты значения, знаковое — с расширением знака
    double f = 0;    // f32 — уже округлено до f32
    bool b = false;
    uint32_t str = 0;  // номер строки в таблице программы
    const Type* type = nullptr;
};

enum class OpKind : uint8_t {
    Copy,   // значение места; там, где значение сохраняется, — копия целиком
    Move,   // значение временного (или его части), временное обнуляется
    Const,
};

struct Operand {
    OpKind kind = OpKind::Const;
    Place place;
    Const c;
};

// --- правые части

enum class BinKind : uint8_t {
    Add, Sub, Mul, Div, Rem,            // целые — с проверкой переполнения и деления на ноль
    AddWrap, SubWrap, MulWrap,          // по модулю 2^N
    BitAnd, BitOr, BitXor, Shl, Shr,    // сдвиг на отрицательное или на ширину и больше — ловушка
    Eq, Ne, Lt, Le, Gt, Ge,             // сравнение значений типа операндов
    Concat,                             // склейка str
};

enum class UnKind : uint8_t { Neg, Not, BitNot };

enum class CastKind : uint8_t {
    Widen,  // неявное расширение без потерь: целое, f32 → f64
    Check,  // T(x) между числами: значение вне T — ловушка
    Wrap,   // T.wrap(x): по модулю 2^N
    ToStr,  // str(x)
};

enum class Intrinsic : uint8_t {
    Len, Repeat, Min, Max, Abs, ParseInt, IsDigit, IsAlpha, IsSpace, IsAlnum, Print, Println, Eprintln,
    // методы List: получатель — изменяемое место (кроме Contains: он — операнд)
    ListPush, ListPop, ListInsert, ListRemove, ListClear, ListSort, ListContains,
    // методы str: получатель — первый операнд
    StrTrim, StrFind, StrStartsWith, StrSplit,
    // внутренние стандартной библиотеки (Р51): аргументы программы и файл;
    // у ReadFile place — строка для текста, ops[0] — путь, результат i32
    ArgsLen, ArgAt, ReadFile,
    // проверяемая арифметика целых (Р53): результат ?T, none — вне типа
    CheckedAdd, CheckedSub, CheckedMul, CheckedDiv, CheckedNeg,
};

enum class RvKind : uint8_t {
    Use,          // ops[0]
    Binary,       // bin ops[0], ops[1]
    Unary,        // un ops[0]
    Cast,         // cast ops[0] к type
    MakeStruct,   // ops — поля по порядку объявления
    MakeVariant,  // variant; ops — данные по порядку объявления
    MakeList,     // ops — элементы; type — List[T]
    MakeSome,     // ops[0]
    MakeNone,
    MakeOk,       // ops[0]; у E!void операндов нет
    MakeErr,      // ops[0] — значение перечисления ошибок
    SliceOf,      // place[ops[0]..ops[1]], без ops[1] — до конца; у List и среза — срез, у str — копия строки
    Call,         // fn; args
    Intrinsic,    // intrinsic; ops; у изменяющего метода List — place
    Tag,          // номер варианта значения place (u32)
    IsSome,       // у place (?T) есть значение
    IsOk,         // place (E!T) — успех
};

struct Arg {
    enum class Kind : uint8_t { Value, Ref, Mut } kind = Kind::Value;
    Operand op;   // Value
    Place place;  // Ref, Mut
};

struct Rvalue {
    RvKind kind = RvKind::Use;
    const Type* type = nullptr;  // тип результата
    BinKind bin = BinKind::Add;
    UnKind un = UnKind::Neg;
    CastKind cast = CastKind::Widen;
    Intrinsic intrinsic = Intrinsic::Len;
    uint32_t variant = 0;
    uint32_t fn = 0;  // номер функции в программе
    std::vector<Operand> ops;
    std::vector<Arg> args;
    Place place;
    bool has_place = false;
};

// --- инструкции и терминаторы

enum class InstKind : uint8_t {
    Assign,  // dst = rv; старое значение dst освобождается после вычисления rv
    Eval,    // rv ради действия; результат, если он есть, освобождается
    Drop,    // освободить переменную dst и обнулить её
};

struct Inst {
    InstKind kind = InstKind::Assign;
    Place dst;
    Rvalue rv;
    Span span;
};

enum class TermKind : uint8_t {
    None,         // блок ещё строится
    Jump,         // then_b
    Branch,       // cond (bool): then_b или else_b
    Return,       // value, если has_value
    Trap,         // ловушка с сообщением message
    Unreachable,  // сюда исполнение не приходит (доказано проверкой программы)
};

struct Term {
    TermKind kind = TermKind::None;
    Operand cond;
    uint32_t then_b = 0, else_b = 0;
    bool has_value = false;
    Operand value;
    std::string message;
    Span span;
};

struct Block {
    std::vector<Inst> insts;
    Term term;
};

struct Function {
    std::string name;  // модуль.функция
    const sema::FnDef* def = nullptr;
    std::vector<Local> locals;  // сначала параметры
    uint32_t param_count = 0;
    const Type* ret = nullptr;  // Void — без результата
    std::vector<Block> blocks;  // вход — блок 0
};

struct Program {
    std::vector<Function> fns;
    std::vector<std::string> strings;
    int main_fn = -1;  // `main` главного модуля; -1 — нет
    const sema::TypeTable* types = nullptr;
};

// Тип места: тип переменной после проекций; nullptr — путь не подходит к типу.
const Type* place_type(const Program& p, const Function& f, const Place& pl);
// Значение типа владеет памятью (внутри есть List или str) и требует drop.
bool needs_drop(const Type* t);
// Тип операнда.
const Type* operand_type(const Program& p, const Function& f, const Operand& op);

}  // namespace dp::ir
