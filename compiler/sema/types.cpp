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

const Type* TypeTable::make(TK k, const Type* elem, const Type* err, const StructDef* s, const EnumDef* e) {
    auto key = std::make_tuple(static_cast<int>(k), static_cast<const void*>(elem), static_cast<const void*>(err),
                               static_cast<const void*>(s), static_cast<const void*>(e));
    auto it = interned_.find(key);
    if (it != interned_.end()) return it->second;
    Type& t = store_.emplace_back();
    t.kind = k;
    t.elem = elem;
    t.err = k == TK::Map ? nullptr : err;
    t.key = k == TK::Map ? err : nullptr;  // у Map ключ лежит в слоте err ключа интернирования
    t.sdef = s;
    t.edef = e;
    interned_.emplace(key, &t);
    return &t;
}

const Type* TypeTable::map(const Type* key, const Type* val) {
    list(key);
    list(val);
    return make(TK::Map, val, key, nullptr, nullptr);
}

const Type* TypeTable::find_list(const Type* elem) const {
    auto it = interned_.find(std::make_tuple(static_cast<int>(TK::List), static_cast<const void*>(elem),
                                             static_cast<const void*>(nullptr), static_cast<const void*>(nullptr),
                                             static_cast<const void*>(nullptr)));
    return it == interned_.end() ? nullptr : it->second;
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
        case TK::Slice: return "[]" + type_name(t->elem, from);
        case TK::Optional: return "?" + type_name(t->elem, from);
        case TK::Result: return (t->err ? type_name(t->err, from) : "") + "!" + type_name(t->elem, from);
        case TK::Struct:
            return from && t->sdef->module != from ? t->sdef->module->name + "." + t->sdef->name : t->sdef->name;
        case TK::Enum:
            return from && t->edef->module != from ? t->edef->module->name + "." + t->edef->name : t->edef->name;
        case TK::Map: return "Map[" + type_name(t->key, from) + ", " + type_name(t->elem, from) + "]";
    }
    return "?";
}

bool contains_slice(const Type* t) {
    if (!t) return false;
    if (t->kind == TK::Slice) return true;
    return contains_slice(t->elem) || contains_slice(t->err) || contains_slice(t->key);
}

bool equatable(const Type* t) {
    switch (t->kind) {
        case TK::Result: case TK::Void: case TK::Map: return false;
        case TK::List: case TK::Slice: case TK::Optional: return equatable(t->elem);
        case TK::Struct:
            for (const FieldDef& f : t->sdef->fields) {
                if (!equatable(f.type)) return false;
            }
            return true;
        case TK::Enum:
            for (const VariantDef& v : t->edef->variants) {
                for (const Type* p : v.types) {
                    if (!equatable(p)) return false;
                }
                for (const FieldDef& f : v.fields) {
                    if (!equatable(f.type)) return false;
                }
            }
            return true;
        default: return true;
    }
}

bool ordered(const Type* t) {
    switch (t->kind) {
        case TK::Error: case TK::Bool: case TK::Str: return true;
        case TK::List: case TK::Slice: return ordered(t->elem);
        case TK::Struct:
            for (const FieldDef& f : t->sdef->fields) {
                if (!ordered(f.type)) return false;
            }
            return true;
        case TK::Enum:
            for (const VariantDef& v : t->edef->variants) {
                for (const Type* p : v.types) {
                    if (!ordered(p)) return false;
                }
                for (const FieldDef& f : v.fields) {
                    if (!ordered(f.type)) return false;
                }
            }
            return true;
        default: return is_numeric(t);
    }
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
