// Типы программы (docs/spec/core.md, раздел 4).
//
// Тип интернируется в таблице одного запуска компилятора: одинаковые
// типы — один объект, сравнение — по указателю.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace dp::ast {
enum class ParamMode : uint8_t;
}

namespace dp::sema {

struct StructDef;
struct EnumDef;
struct HandleDef;

enum class TK : uint8_t {
    Error,  // тип ошибочного выражения: дальше сообщений о нём нет
    Void,   // нет значения: функция без результата, `!void`
    Bool,
    I8, I16, I32, I64, Int, U8, U16, U32, U64,
    F32, F64,
    Str,
    List,      // List[T]
    Map,       // Map[K, V] (Р54): key — ключ, elem — значение
    Pool,        // Pool[T] (Р67): elem — элемент
    PoolHandle,  // Handle[T] (Р67): номер слота и поколение; elem — элемент пула
    Slice,     // []T
    Optional,  // ?T
    Result,    // E!T; err — перечисление ошибок
    Struct,
    Enum,
    Handle,    // `extern type` — непрозрачный дескриптор C (Р59); hdef
    CFn,       // `extern fn(T) R` — указатель на функцию C (Р61); sig
    Array,     // `[N]T` — массив C (Р62): elem, len; поле `@c`
    Ref,       // `&T` — заимствующее поле `@c` или `?&T` параметра функции C (Р62); elem
};

struct Type;

// Сигнатура типа `extern fn(...)`: режимы и типы параметров, результат
// (Void — без результата). Имена параметров в тип не входят (Р61).
struct CFnSig {
    std::vector<std::pair<ast::ParamMode, const Type*>> params;
    const Type* ret = nullptr;
};

struct Type {
    TK kind = TK::Error;
    const Type* elem = nullptr;  // List, Slice, Optional, Pool, Handle — элемент; Result, Map — значение
    const Type* key = nullptr;   // Map — ключ
    const Type* err = nullptr;   // Result — тип ошибки (перечисление); nullptr — ещё не выведен (Р41)
    const StructDef* sdef = nullptr;
    const EnumDef* edef = nullptr;
    const HandleDef* hdef = nullptr;
    const CFnSig* sig = nullptr;  // CFn
    uint64_t len = 0;             // Array
};

class TypeTable {
public:
    TypeTable();
    const Type* prim(TK k) const { return prims_[static_cast<size_t>(k)]; }
    const Type* error() const { return prim(TK::Error); }
    const Type* list(const Type* elem) { return make(TK::List, elem, nullptr, nullptr, nullptr); }
    const Type* map(const Type* key, const Type* value) { return make(TK::Map, value, nullptr, nullptr, nullptr, key); }
    const Type* pool(const Type* elem) { return make(TK::Pool, elem, nullptr, nullptr, nullptr); }
    const Type* pool_handle(const Type* elem) { return make(TK::PoolHandle, elem, nullptr, nullptr, nullptr); }
    const Type* slice(const Type* elem) { return make(TK::Slice, elem, nullptr, nullptr, nullptr); }
    const Type* optional(const Type* elem) { return make(TK::Optional, elem, nullptr, nullptr, nullptr); }
    const Type* result(const Type* err, const Type* value) { return make(TK::Result, value, err, nullptr, nullptr); }
    const Type* struct_type(const StructDef* d) { return make(TK::Struct, nullptr, nullptr, d, nullptr); }
    const Type* enum_type(const EnumDef* d) { return make(TK::Enum, nullptr, nullptr, nullptr, d); }
    const Type* handle(const HandleDef* d) { return make(TK::Handle, nullptr, nullptr, nullptr, nullptr, nullptr, d); }
    const Type* cfn(const CFnSig& sig);
    const Type* array(const Type* elem, uint64_t len);
    const Type* ref(const Type* elem) { return make(TK::Ref, elem, nullptr, nullptr, nullptr); }

private:
    const Type* make(TK k, const Type* elem, const Type* err, const StructDef* s, const EnumDef* e,
                     const Type* key = nullptr, const HandleDef* h = nullptr);
    std::deque<Type> store_;
    const Type* prims_[static_cast<size_t>(TK::Str) + 1] = {};
    std::map<std::tuple<int, const void*, const void*, const void*, const void*, const void*, const void*>, const Type*>
        interned_;
    std::deque<CFnSig> sigs_;
    std::map<std::pair<const Type*, uint64_t>, const Type*> arrays_;
    std::map<std::pair<std::vector<std::pair<int, const Type*>>, const Type*>, const Type*> cfns_;
};

struct ModuleInfo;

// Запись типа в сообщениях: `List[i32]`, `ReadError!str`, `?world.Unit`.
// Тип из модуля, отличного от from, пишется с именем модуля; без from — без него.
std::string type_name(const Type* t, const ModuleInfo* from = nullptr);

// Тип содержит срез `[]T` где-либо внутри (Р44: срез — только тип параметра).
bool contains_slice(const Type* t);

// Заимствующее поле `@c` (Р62): str, []T, &T, ?&T — в C переводится при
// вызове. Заимствующая структура — `@c` с таким полем
// (или с полем-заимствующей структурой): живёт только в аргументе вызова.
bool c_borrow_field(const Type* t);
bool c_borrowing(const Type* t);
// Значение хранит вид до вызова, который его поглотит: срез, `&T`,
// заимствующая структура `@c` (Р62) или список таких (массив для C).
bool holds_view(const Type* t);
// Тип поля `@c`: вид в C или заимствующее поле.
bool c_field_type(const Type* t);
// У типа есть ноль (Р62): пропущенное поле `@c` — ноль. Числа, bool, ?T,
// str и срез (пустые), массив и `@c`-структура из таких.
bool zeroable(const Type* t);
// Общее начало (Р62): поля `@c`-структуры b — начало полей s с теми же
// типами; ссылка `&s` тогда переходит в `&b`.
bool c_prefix(const Type* b, const Type* s);

// Тип содержит массив `[N]T` или ссылку-поле `&T` (Р62): им место — в
// полях `@c`-структур (и `?&T` — в параметре функции C).
bool contains_c_only(const Type* t);

// Тип годится в ключ Map (Р54): целое, bool или str.
bool map_key_type(const Type* t);

// Линейный тип (Р60): структура `@linear`, ?T и E!T с линейным T.
bool linear_type(const Type* t);
// Линейное значение в List, Map или срезе где-либо в типе (E0514, Р60).
bool linear_in_container(const Type* t);

// Значение типа переходит границу C как есть (Р59): целое, плавающее,
// bool, дескриптор `extern type` и `?H`, `extern fn(...)` и `?extern fn`
// (указатель или NULL), массив и структура `@c` из таких.
bool c_value_type(const Type* t);

// Тип параметра обратного вызова `extern fn(...)` и функции `@export`
// (Р61): по значению — вид в C, str, ?H, ?extern fn; по ссылке — вид в C.
bool export_param(const Type* t, ast::ParamMode mode);
// Результат там же: нет результата, вид в C, ?H, ?extern fn.
bool export_result(const Type* t);

bool is_integer(const Type* t);
bool is_signed(const Type* t);
int int_bits(const Type* t);  // ширина целого; int — 64
bool is_float(const Type* t);
bool is_numeric(const Type* t);

}  // namespace dp::sema
