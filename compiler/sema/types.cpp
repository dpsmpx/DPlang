#include "sema/types.h"

#include "sema/defs.h"

namespace dp::sema {

TypeTable::TypeTable() {
    for (size_t i = 0; i <= static_cast<size_t>(TK::Str); i++) {
        Type& t = store_.emplace_back();
        t.kind = static_cast<TK>(i);
        prims_[i] = &t;
    }
}

const Type* TypeTable::make(TK k, const Type* elem, const Type* err, const StructDef* s, const EnumDef* e,
                            const Type* key_type, const HandleDef* h) {
    auto key = std::make_tuple(static_cast<int>(k), static_cast<const void*>(elem), static_cast<const void*>(err),
                               static_cast<const void*>(s), static_cast<const void*>(e),
                               static_cast<const void*>(key_type), static_cast<const void*>(h));
    auto it = interned_.find(key);
    if (it != interned_.end()) return it->second;
    Type& t = store_.emplace_back();
    t.kind = k;
    t.elem = elem;
    t.err = err;
    t.sdef = s;
    t.edef = e;
    t.key = key_type;
    t.hdef = h;
    interned_.emplace(key, &t);
    return &t;
}

const Type* TypeTable::cfn(const CFnSig& sig) {
    std::vector<std::pair<int, const Type*>> params;
    for (const auto& [mode, t] : sig.params) params.emplace_back(static_cast<int>(mode), t);
    auto key = std::make_pair(std::move(params), sig.ret);
    auto it = cfns_.find(key);
    if (it != cfns_.end()) return it->second;
    Type& t = store_.emplace_back();
    t.kind = TK::CFn;
    t.sig = &sigs_.emplace_back(sig);
    cfns_.emplace(std::move(key), &t);
    return &t;
}

const Type* TypeTable::array(const Type* elem, uint64_t len) {
    auto key = std::make_pair(elem, len);
    auto it = arrays_.find(key);
    if (it != arrays_.end()) return it->second;
    Type& t = store_.emplace_back();
    t.kind = TK::Array;
    t.elem = elem;
    t.len = len;
    arrays_.emplace(key, &t);
    return &t;
}

bool contains_c_only(const Type* t) {
    if (!t) return false;
    if (t->kind == TK::Array || t->kind == TK::Ref) return true;
    if (t->kind == TK::CFn) return false;  // сигнатура проверяется сама (check_cfn)
    return contains_c_only(t->elem) || contains_c_only(t->err) || contains_c_only(t->key);
}

std::string type_name(const Type* t, const ModuleInfo* from) {
    if (!t) return "?";
    switch (t->kind) {
        case TK::Error: return "<error>";
        case TK::Void: return "void";
        case TK::Bool: return "bool";
        case TK::I8: return "i8";
        case TK::I16: return "i16";
        case TK::I32: return "i32";
        case TK::I64: return "i64";
        case TK::Int: return "int";
        case TK::U8: return "u8";
        case TK::U16: return "u16";
        case TK::U32: return "u32";
        case TK::U64: return "u64";
        case TK::F32: return "f32";
        case TK::F64: return "f64";
        case TK::Str: return "str";
        case TK::List: return "List[" + type_name(t->elem, from) + "]";
        case TK::Map: return "Map[" + type_name(t->key, from) + ", " + type_name(t->elem, from) + "]";
        case TK::Slice: return "[]" + type_name(t->elem, from);
        case TK::Optional: return (t->elem->kind == TK::Optional ? "? " : "?") + type_name(t->elem, from);
        case TK::Result: return (t->err ? type_name(t->err, from) : "") + "!" + type_name(t->elem, from);
        case TK::Struct:
            return from && t->sdef->module != from ? t->sdef->module->name + "." + t->sdef->name : t->sdef->name;
        case TK::Enum:
            return from && t->edef->module != from ? t->edef->module->name + "." + t->edef->name : t->edef->name;
        case TK::Handle:
            return from && t->hdef->module != from ? t->hdef->module->name + "." + t->hdef->name : t->hdef->name;
        case TK::Array: return "[" + std::to_string(t->len) + "]" + type_name(t->elem, from);
        case TK::Ref: return "&" + type_name(t->elem, from);
        case TK::CFn: {
            std::string s = "extern fn(";
            for (size_t i = 0; i < t->sig->params.size(); i++) {
                const auto& [mode, p] = t->sig->params[i];
                s += std::string(i ? ", " : "") +
                     (mode == ast::ParamMode::Ref ? "&" : mode == ast::ParamMode::Mut ? "&mut " : "") +
                     type_name(p, from);
            }
            return s + ")" + (t->sig->ret->kind == TK::Void ? "" : " " + type_name(t->sig->ret, from));
        }
    }
    return "?";
}

bool contains_slice(const Type* t) {
    if (!t) return false;
    if (t->kind == TK::Slice) return true;
    return contains_slice(t->elem) || contains_slice(t->err) || contains_slice(t->key);
}

bool linear_type(const Type* t) {
    if (!t) return false;
    if (t->kind == TK::Struct) return t->sdef->linear;
    if (t->kind == TK::Optional || t->kind == TK::Result) return linear_type(t->elem);
    return false;
}

bool linear_in_container(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TK::List: case TK::Slice: return linear_type(t->elem) || linear_in_container(t->elem);
        case TK::Map: return linear_type(t->elem) || linear_in_container(t->elem);
        case TK::Optional: return linear_in_container(t->elem);
        case TK::Result: return linear_in_container(t->elem) || linear_in_container(t->err);
        default: return false;
    }
}

bool map_key_type(const Type* t) { return t && (is_integer(t) || t->kind == TK::Bool || t->kind == TK::Str); }

bool c_value_type(const Type* t) {
    if (!t) return false;
    if (is_numeric(t) || t->kind == TK::Bool || t->kind == TK::Handle || t->kind == TK::CFn) return true;
    if (t->kind == TK::Array) return c_value_type(t->elem);
    if (t->kind != TK::Struct || !t->sdef->c) return false;
    for (const FieldDef& f : t->sdef->fields) {
        if (!c_value_type(f.type)) return false;
    }
    return true;
}

bool c_borrow_field(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TK::Str: return true;
        case TK::Slice: return c_value_type(t->elem) || t->elem->kind == TK::Str || c_borrowing(t->elem);
        case TK::Ref: return c_value_type(t->elem) || c_borrowing(t->elem);
        case TK::Optional:
            return t->elem->kind == TK::Handle || t->elem->kind == TK::CFn ||
                   (t->elem->kind == TK::Ref && c_borrow_field(t->elem));
        default: return false;
    }
}

// Поле — заимствующая структура по значению: рекурсия только по значению,
// а такие циклы — E0423; глубина ограничена на случай ещё не найденного цикла.
static bool c_borrowing_at(const Type* t, int depth) {
    if (!t || t->kind != TK::Struct || !t->sdef->c || depth > 64) return false;
    for (const FieldDef& f : t->sdef->fields) {
        if (c_borrow_field(f.type) || c_borrowing_at(f.type, depth + 1)) return true;
    }
    return false;
}

bool c_borrowing(const Type* t) { return c_borrowing_at(t, 0); }

bool c_field_type(const Type* t) { return c_value_type(t) || c_borrow_field(t) || c_borrowing(t); }

static bool zeroable_at(const Type* t, int depth) {
    if (!t || depth > 64) return false;
    if (is_numeric(t) || t->kind == TK::Bool || t->kind == TK::Optional || t->kind == TK::Str || t->kind == TK::Slice) {
        return true;
    }
    if (t->kind == TK::Array) return zeroable_at(t->elem, depth + 1);
    if (t->kind != TK::Struct || !t->sdef->c) return false;
    for (const FieldDef& f : t->sdef->fields) {
        if (!zeroable_at(f.type, depth + 1)) return false;
    }
    return true;
}

bool zeroable(const Type* t) { return zeroable_at(t, 0); }

// ?H и ?extern fn — указатель или NULL.
static bool nullable_pointer(const Type* t) {
    return t->kind == TK::Optional && (t->elem->kind == TK::Handle || t->elem->kind == TK::CFn);
}

bool export_param(const Type* t, ast::ParamMode mode) {
    if (!t || t->kind == TK::Error) return true;
    if (t->kind == TK::Array) return false;  // массив C по значению не передаётся
    if (mode != ast::ParamMode::Value) return c_value_type(t);
    return c_value_type(t) || t->kind == TK::Str || nullable_pointer(t);
}

bool export_result(const Type* t) {
    if (!t || t->kind == TK::Error || t->kind == TK::Void) return true;
    if (t->kind == TK::Array) return false;
    return c_value_type(t) || nullable_pointer(t);
}

bool is_integer(const Type* t) { return t && t->kind >= TK::I8 && t->kind <= TK::U64; }
bool is_signed(const Type* t) { return t && t->kind >= TK::I8 && t->kind <= TK::Int; }
bool is_float(const Type* t) { return t && (t->kind == TK::F32 || t->kind == TK::F64); }
bool is_numeric(const Type* t) { return is_integer(t) || is_float(t); }

int int_bits(const Type* t) {
    switch (t->kind) {
        case TK::I8: case TK::U8: return 8;
        case TK::I16: case TK::U16: return 16;
        case TK::I32: case TK::U32: return 32;
        default: return 64;
    }
}

}  // namespace dp::sema
