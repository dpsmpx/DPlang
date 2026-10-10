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
    Slice,     // []T
    Optional,  // ?T
    Result,    // E!T; err — перечисление ошибок
    Struct,
    Enum,
    Handle,    // `extern type` — непрозрачный дескриптор C (Р59); hdef
};

struct Type {
    TK kind = TK::Error;
    const Type* elem = nullptr;  // List, Slice, Optional — элемент; Result, Map — значение
    const Type* key = nullptr;   // Map — ключ
    const Type* err = nullptr;   // Result — тип ошибки (перечисление); nullptr — ещё не выведен (Р41)
    const StructDef* sdef = nullptr;
    const EnumDef* edef = nullptr;
    const HandleDef* hdef = nullptr;
};

class TypeTable {
public:
    TypeTable();
    const Type* prim(TK k) const { return prims_[static_cast<size_t>(k)]; }
    const Type* error() const { return prim(TK::Error); }
    const Type* list(const Type* elem) { return make(TK::List, elem, nullptr, nullptr, nullptr); }
    const Type* map(const Type* key, const Type* value) { return make(TK::Map, value, nullptr, nullptr, nullptr, key); }
    const Type* slice(const Type* elem) { return make(TK::Slice, elem, nullptr, nullptr, nullptr); }
    const Type* optional(const Type* elem) { return make(TK::Optional, elem, nullptr, nullptr, nullptr); }
    const Type* result(const Type* err, const Type* value) { return make(TK::Result, value, err, nullptr, nullptr); }
    const Type* struct_type(const StructDef* d) { return make(TK::Struct, nullptr, nullptr, d, nullptr); }
    const Type* enum_type(const EnumDef* d) { return make(TK::Enum, nullptr, nullptr, nullptr, d); }
    const Type* handle(const HandleDef* d) { return make(TK::Handle, nullptr, nullptr, nullptr, nullptr, nullptr, d); }

private:
    const Type* make(TK k, const Type* elem, const Type* err, const StructDef* s, const EnumDef* e,
                     const Type* key = nullptr, const HandleDef* h = nullptr);
    std::deque<Type> store_;
    const Type* prims_[static_cast<size_t>(TK::Str) + 1] = {};
    std::map<std::tuple<int, const void*, const void*, const void*, const void*, const void*, const void*>, const Type*>
        interned_;
};

struct ModuleInfo;

// Запись типа в сообщениях: `List[i32]`, `ReadError!str`, `?world.Unit`.
// Тип из модуля, отличного от from, пишется с именем модуля; без from — без него.
std::string type_name(const Type* t, const ModuleInfo* from = nullptr);

// Тип содержит срез `[]T` где-либо внутри (Р44: срез — только тип параметра).
bool contains_slice(const Type* t);

// Тип годится в ключ Map (Р54): целое, bool или str.
bool map_key_type(const Type* t);

// Значение типа переходит границу C как есть (Р59): целое, плавающее,
// bool, дескриптор `extern type`, структура `@c` из таких.
bool c_value_type(const Type* t);

bool is_integer(const Type* t);
bool is_signed(const Type* t);
int int_bits(const Type* t);  // ширина целого; int — 64
bool is_float(const Type* t);
bool is_numeric(const Type* t);

}  // namespace dp::sema
