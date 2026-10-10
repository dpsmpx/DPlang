// C-бэкенд: Core IR → C11 (docs/compiler/backend_c.md).
//
// Смысл каждой операции — как у эталонного интерпретатора
// (compiler/ir/interp.cpp): различающие тесты сравнивают вывод нативной
// программы с его выводом байт в байт.
//
// Порядок вычисления в C у аргументов функции и частей выражения не задан.
// Поэтому каждый операнд вычисляется отдельной инструкцией во временную
// переменную в порядке IR, а место с индексом — через указатель-временное:
// ловушки срабатывают в том же порядке, что у интерпретатора.
#include "backend_c/emit_c.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace dp::backend_c {

using ir::Arg;
using ir::BinKind;
using ir::CastKind;
using ir::Const;
using ir::Function;
using ir::Inst;
using ir::InstKind;
using ir::Intrinsic;
using ir::LocalKind;
using ir::OpKind;
using ir::Operand;
using ir::Place;
using ir::ProjKind;
using ir::Rvalue;
using ir::RvKind;
using ir::Term;
using ir::TermKind;
using sema::TK;
using sema::Type;

namespace {

bool is_scalar(const Type* t) {
    switch (t->kind) {
        case TK::Bool: case TK::I8: case TK::I16: case TK::I32: case TK::I64: case TK::Int: case TK::U8:
        case TK::U16: case TK::U32: case TK::U64: case TK::F32: case TK::F64:
            return true;
        default: return false;
    }
}

bool is_composite(const Type* t) {
    switch (t->kind) {
        case TK::List: case TK::Map: case TK::Slice: case TK::Optional: case TK::Result: case TK::Struct: case TK::Enum:
            return true;
        default: return false;
    }
}

// Имя dpl в имени C — с длиной впереди: «a» и «__x» дают 1a3__x, а
// «a__» и «x» — 3a__1x; без длины оба дали бы одно имя.
std::string lp(const std::string& s) { return std::to_string(s.size()) + s; }

// Запись типа в именах C: i32, str, L_i32, O_str, R_E_1m3Err_void, T_1m1S.
// Записи однозначны: по записи восстанавливается тип.
std::string mangle(const Type* t) {
    switch (t->kind) {
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
        case TK::List: return "L_" + mangle(t->elem);
        case TK::Map: return "M_" + mangle(t->key) + "_" + mangle(t->elem);
        case TK::Slice: return "S_" + mangle(t->elem);
        case TK::Optional: return "O_" + mangle(t->elem);
        case TK::Result: return "R_" + (t->err ? mangle(t->err) : std::string("x")) + "_" + mangle(t->elem);
        case TK::Struct: return "T_" + lp(t->sdef->module->name) + lp(t->sdef->name);
        case TK::Enum: return "E_" + lp(t->edef->module->name) + lp(t->edef->name);
        default: return "error";
    }
}

std::string ctype(const Type* t) {
    switch (t->kind) {
        case TK::Void: return "void";
        case TK::Bool: return "bool";
        case TK::I8: return "int8_t";
        case TK::I16: return "int16_t";
        case TK::I32: return "int32_t";
        case TK::I64: case TK::Int: return "int64_t";
        case TK::U8: return "uint8_t";
        case TK::U16: return "uint16_t";
        case TK::U32: return "uint32_t";
        case TK::U64: return "uint64_t";
        case TK::F32: return "float";
        case TK::F64: return "double";
        case TK::Str: return "dp_str";
        default: return "dp_" + mangle(t);
    }
}

std::string cstr(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7f && c != '?') {
            out += static_cast<char>(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", c);
            out += buf;
        }
    }
    return out + "\"";
}

const Type* payload_type(const Type* t, uint32_t variant, uint32_t k) {
    const sema::VariantDef& v = t->edef->variants[variant];
    return v.form == ast::VariantForm::Tuple ? v.types[k] : v.fields[k].type;
}

size_t payload_count(const sema::VariantDef& v) {
    return v.form == ast::VariantForm::Tuple ? v.types.size() : v.fields.size();
}

bool has_payload(const Type* t) {
    for (const sema::VariantDef& v : t->edef->variants) {
        if (payload_count(v) > 0) return true;
    }
    return false;
}

// --- выражения по типу (указатели a и b — выражения C)

std::string eq_of(const Type* t, const std::string& a, const std::string& b) {
    if (is_scalar(t)) return "(*(" + a + ") == *(" + b + "))";
    if (t->kind == TK::Str) return "dp_str_eq(*(" + a + "), *(" + b + "))";
    return "dp_eq__" + mangle(t) + "(" + a + ", " + b + ")";
}

std::string lt_of(const Type* t, const std::string& a, const std::string& b) {
    if (is_scalar(t)) return "(*(" + a + ") < *(" + b + "))";
    if (t->kind == TK::Str) return "dp_str_lt(*(" + a + "), *(" + b + "))";
    // ?T и E!T интерпретатор не упорядочивает: «меньше» всегда ложно.
    if (t->kind == TK::Optional || t->kind == TK::Result) return "false";
    return "dp_lt__" + mangle(t) + "(" + a + ", " + b + ")";
}

// Копия значения места lv: у значений с памятью — глубокая.
std::string copy_of(const Type* t, const std::string& lv) {
    if (!ir::needs_drop(t)) return lv;
    if (t->kind == TK::Str) return "dp_str_copy(&(" + lv + "))";
    return "dp_copy__" + mangle(t) + "(&(" + lv + "))";
}

// Освобождение значения места lv с обнулением; пусто — без памяти.
std::string drop_of(const Type* t, const std::string& lv) {
    if (!ir::needs_drop(t)) return "";
    if (t->kind == TK::Str) return "dp_str_drop(&(" + lv + "));";
    return "dp_drop__" + mangle(t) + "(&(" + lv + "));";
}

class Emitter {
public:
    Emitter(const ir::Program& p, const SourceMap& sources, const std::vector<uint32_t>* tests)
        : p_(p), src_(sources), tests_(tests) {}

    CResult run() {
        for (const Function& f : p_.fns) collect(f);
        std::string out;
        out += "// Сгенерировано C-бэкендом dpl — не править.\n";
        out += "#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n#include \"dp_rt.h\"\n\n";
        out += strings();
        out += type_decls();
        out += helpers();
        for (const Function& f : p_.fns) out += fn_head(f) + ";\n";
        out += "\n";
        for (const Function& f : p_.fns) out += function(f);
        out += tests_ ? test_entry() : entry();
        return CResult{out, errors_};
    }

    // Для FnEmitter.
    // Имя функции в IR — «модуль.функция»; в C — dpf_<модуль><функция> с длинами.
    static std::string fn_name(const Function& f) {
        size_t dot = f.name.find('.');
        if (dot == std::string::npos) return "dpf_" + lp(f.name);
        return "dpf_" + lp(f.name.substr(0, dot)) + lp(f.name.substr(dot + 1));
    }

    std::string fn_head(const Function& f) {
        std::string h = "DP_FN " + ctype(f.ret) + " " + fn_name(f) + "(";
        for (uint32_t i = 0; i < f.param_count; i++) {
            if (i) h += ", ";
            const ir::Local& l = f.locals[i];
            bool ref = l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut;
            h += ctype(l.type) + (ref ? "* " : " ") + "l" + std::to_string(i) + (l.name.empty() ? "" : "_" + l.name);
        }
        if (f.param_count == 0) h += "void";
        return h + ")";
    }

    std::string loc(const Span& s) {
        if (s.file >= src_.size()) return "\"\"";
        const SourceFile& file = src_.file(s.file);
        LineCol lc = file.line_col(s.begin);
        return cstr(file.path() + ":" + std::to_string(lc.line) + ":" + std::to_string(lc.col));
    }

    std::string line_directive(const Span& s) {
        if (s.file >= src_.size()) return "";
        const SourceFile& file = src_.file(s.file);
        return "#line " + std::to_string(file.line_col(s.begin).line) + " " + cstr(file.path()) + "\n";
    }

private:
    const ir::Program& p_;
    const SourceMap& src_;
    const std::vector<uint32_t>* tests_ = nullptr;  // сборка тестов: функции-тесты по порядку
    std::vector<std::string> errors_;
    std::vector<const Type*> types_;  // составные типы в порядке определения
    std::set<const Type*> seen_;
    std::set<std::string> mangled_;

    void error(const std::string& msg) { errors_.push_back(msg); }

    // --- сбор типов

    void add_type(const Type* t) {
        if (!t || seen_.count(t)) return;
        seen_.insert(t);
        switch (t->kind) {
            case TK::List: case TK::Slice: add_type(t->elem); break;
            case TK::Map:
                add_type(t->key);
                add_type(t->elem);
                break;
            case TK::Optional: add_type(t->elem); break;
            case TK::Result:
                add_type(t->elem);
                add_type(t->err);
                if (!t->err) error("result type without an error type");
                break;
            case TK::Struct:
                for (const sema::FieldDef& f : t->sdef->fields) add_type(f.type);
                break;
            case TK::Enum:
                for (uint32_t v = 0; v < t->edef->variants.size(); v++) {
                    for (uint32_t k = 0; k < payload_count(t->edef->variants[v]); k++) add_type(payload_type(t, v, k));
                }
                break;
            default: break;
        }
        if (is_composite(t)) {
            types_.push_back(t);
            mangled_.insert(mangle(t));
        }
    }

    void collect(const Function& f) {
        add_type(f.ret);
        for (const ir::Local& l : f.locals) add_type(l.type);
        for (const ir::Block& b : f.blocks) {
            for (const Inst& in : b.insts) {
                add_type(in.rv.type);
                for (const Operand& o : in.rv.ops) {
                    if (o.kind == OpKind::Const) add_type(o.c.type);
                }
            }
        }
    }

    // --- строки программы

    std::string strings() {
        std::string out;
        for (size_t i = 0; i < p_.strings.size(); i++) {
            const std::string& s = p_.strings[i];
            out += "static const uint8_t dp_s" + std::to_string(i) + "[" + std::to_string(s.size() + 1) + "] = {";
            for (unsigned char c : s) out += std::to_string(c) + ",";
            out += "0};\n";
        }
        return out + "\n";
    }

    // --- типы

    std::string type_decls() {
        std::string out;
        for (const Type* t : types_) out += "typedef struct " + ctype(t) + " " + ctype(t) + ";\n";
        out += "\n";
        for (const Type* t : types_) out += type_def(t);
        return out;
    }

    std::string type_def(const Type* t) {
        std::string n = ctype(t);
        std::string out = "struct " + n + " {\n";
        switch (t->kind) {
            case TK::List: out += "    " + ctype(t->elem) + "* ptr;\n    int64_t len;\n    int64_t cap;\n"; break;
            case TK::Map:  // Р54: пары в порядке вставки и хэш-индекс позиций (позиция + 1, 0 — пусто)
                out += "    " + ctype(t->key) + "* keys;\n    " + ctype(t->elem) +
                       "* vals;\n    int64_t len;\n    int64_t cap;\n    int64_t* slots;\n    int64_t nslots;\n";
                break;
            case TK::Slice: out += "    " + ctype(t->elem) + "* ptr;\n    int64_t len;\n"; break;
            case TK::Optional: out += "    bool has;\n    " + ctype(t->elem) + " value;\n"; break;
            case TK::Result:
                out += "    bool is_err;\n";
                if (t->elem->kind != TK::Void) out += "    " + ctype(t->elem) + " value;\n";
                if (t->err) out += "    " + ctype(t->err) + " err;\n";
                break;
            case TK::Struct:
                if (t->sdef->fields.empty()) out += "    char dp_empty;\n";
                for (const sema::FieldDef& f : t->sdef->fields) out += "    " + ctype(f.type) + " f_" + f.name + ";\n";
                break;
            case TK::Enum:
                out += "    uint32_t tag;\n";
                if (has_payload(t)) {
                    out += "    union {\n";
                    for (uint32_t v = 0; v < t->edef->variants.size(); v++) {
                        const sema::VariantDef& vd = t->edef->variants[v];
                        if (payload_count(vd) == 0) continue;
                        out += "        struct {";
                        for (uint32_t k = 0; k < payload_count(vd); k++) {
                            out += " " + ctype(payload_type(t, v, k)) + " p" + std::to_string(k) + ";";
                        }
                        out += " } v_" + vd.name + ";\n";
                    }
                    out += "    } u;\n";
                }
                break;
            default: break;
        }
        return out + "};\n\n";
    }

    // --- функции типов: eq, lt, drop, copy; методы List

    std::string helpers() {
        std::string decl, def;
        for (const Type* t : types_) {
            std::string n = ctype(t), m = mangle(t);
            decl += "DP_INLINE bool dp_eq__" + m + "(const " + n + "* a, const " + n + "* b);\n";
            decl += "DP_INLINE bool dp_lt__" + m + "(const " + n + "* a, const " + n + "* b);\n";
            def += eq_def(t) + lt_def(t);
            if (ir::needs_drop(t)) {
                decl += "DP_INLINE void dp_drop__" + m + "(" + n + "* v);\n";
                decl += "DP_INLINE " + n + " dp_copy__" + m + "(const " + n + "* v);\n";
                def += drop_def(t) + copy_def(t);
            }
        }
        std::set<std::string> contains_done;
        for (const Type* t : types_) {
            if (t->kind != TK::List && t->kind != TK::Slice) continue;
            std::string em = mangle(t->elem);
            if (!contains_done.count(em)) {
                contains_done.insert(em);
                std::string ec = ctype(t->elem);
                def += "DP_INLINE bool dp_contains__" + em + "(const " + ec + "* p, int64_t n, const " + ec +
                       "* x) {\n    for (int64_t i = 0; i < n; i++) {\n        if (" + eq_of(t->elem, "&p[i]", "x") +
                       ") return true;\n    }\n    return false;\n}\n\n";
            }
            if (t->kind == TK::List) def += list_methods(t);
        }
        for (const Type* t : types_) {
            if (t->kind == TK::Map) def += map_methods(t);
        }
        return decl + "\n" + def;
    }

    std::string eq_def(const Type* t) {
        std::string n = ctype(t), m = mangle(t);
        std::string b = "DP_INLINE bool dp_eq__" + m + "(const " + n + "* a, const " + n + "* b) {\n";
        switch (t->kind) {
            case TK::List: case TK::Slice:
                b += "    if (a->len != b->len) return false;\n    for (int64_t i = 0; i < a->len; i++) {\n        if (!" +
                     eq_of(t->elem, "&a->ptr[i]", "&b->ptr[i]") + ") return false;\n    }\n    return true;\n";
                break;
            case TK::Optional:
                b += "    if (a->has != b->has) return false;\n    return !a->has || " +
                     eq_of(t->elem, "&a->value", "&b->value") + ";\n";
                break;
            case TK::Result: b += "    (void)a;\n    (void)b;\n    return true;\n"; break;
            case TK::Map: b += "    (void)a;\n    (void)b;\n    return false;\n"; break;  // не сравниваются (Р54)
            case TK::Struct:
                if (t->sdef->fields.empty()) b += "    (void)a;\n    (void)b;\n";
                for (const sema::FieldDef& f : t->sdef->fields) {
                    b += "    if (!" + eq_of(f.type, "&a->f_" + f.name, "&b->f_" + f.name) + ") return false;\n";
                }
                b += "    return true;\n";
                break;
            case TK::Enum:
                b += "    if (a->tag != b->tag) return false;\n";
                if (has_payload(t)) {
                    b += "    switch (a->tag) {\n";
                    for (uint32_t v = 0; v < t->edef->variants.size(); v++) {
                        const sema::VariantDef& vd = t->edef->variants[v];
                        if (payload_count(vd) == 0) continue;
                        b += "        case " + std::to_string(v) + ":\n";
                        for (uint32_t k = 0; k < payload_count(vd); k++) {
                            std::string fld = "u.v_" + vd.name + ".p" + std::to_string(k);
                            b += "            if (!" + eq_of(payload_type(t, v, k), "&a->" + fld, "&b->" + fld) +
                                 ") return false;\n";
                        }
                        b += "            break;\n";
                    }
                    b += "        default: break;\n    }\n";
                }
                b += "    return true;\n";
                break;
            default: break;
        }
        return b + "}\n\n";
    }

    std::string lt_parts(const std::vector<std::pair<const Type*, std::string>>& parts, const std::string& ind) {
        std::string b;
        for (const auto& [pt, fld] : parts) {
            b += ind + "if (" + lt_of(pt, "&a->" + fld, "&b->" + fld) + ") return true;\n";
            b += ind + "if (" + lt_of(pt, "&b->" + fld, "&a->" + fld) + ") return false;\n";
        }
        return b;
    }

    std::string lt_def(const Type* t) {
        std::string n = ctype(t), m = mangle(t);
        std::string b = "DP_INLINE bool dp_lt__" + m + "(const " + n + "* a, const " + n + "* b) {\n";
        switch (t->kind) {
            case TK::List: case TK::Slice:
                b += "    int64_t n = a->len < b->len ? a->len : b->len;\n    for (int64_t i = 0; i < n; i++) {\n";
                b += "        if (" + lt_of(t->elem, "&a->ptr[i]", "&b->ptr[i]") + ") return true;\n";
                b += "        if (" + lt_of(t->elem, "&b->ptr[i]", "&a->ptr[i]") + ") return false;\n";
                b += "    }\n    return a->len < b->len;\n";
                break;
            case TK::Optional: case TK::Result: case TK::Map: b += "    (void)a;\n    (void)b;\n    return false;\n"; break;
            case TK::Struct: {
                std::vector<std::pair<const Type*, std::string>> parts;
                for (const sema::FieldDef& f : t->sdef->fields) parts.push_back({f.type, "f_" + f.name});
                if (parts.empty()) b += "    (void)a;\n    (void)b;\n";
                b += lt_parts(parts, "    ") + "    return false;\n";
                break;
            }
            case TK::Enum:
                b += "    if (a->tag != b->tag) return a->tag < b->tag;\n";
                if (has_payload(t)) {
                    b += "    switch (a->tag) {\n";
                    for (uint32_t v = 0; v < t->edef->variants.size(); v++) {
                        const sema::VariantDef& vd = t->edef->variants[v];
                        if (payload_count(vd) == 0) continue;
                        std::vector<std::pair<const Type*, std::string>> parts;
                        for (uint32_t k = 0; k < payload_count(vd); k++) {
                            parts.push_back({payload_type(t, v, k), "u.v_" + vd.name + ".p" + std::to_string(k)});
                        }
                        b += "        case " + std::to_string(v) + ":\n" + lt_parts(parts, "            ") +
                             "            break;\n";
                    }
                    b += "        default: break;\n    }\n";
                }
                b += "    return false;\n";
                break;
            default: break;
        }
        return b + "}\n\n";
    }

    std::string drop_def(const Type* t) {
        std::string n = ctype(t), m = mangle(t);
        std::string b = "DP_INLINE void dp_drop__" + m + "(" + n + "* v) {\n";
        auto drop_line = [&](const Type* pt, const std::string& lv, const std::string& ind) {
            std::string d = drop_of(pt, lv);
            return d.empty() ? std::string() : ind + d + "\n";
        };
        switch (t->kind) {
            case TK::List:
                if (ir::needs_drop(t->elem)) {
                    b += "    for (int64_t i = 0; i < v->len; i++) " + drop_of(t->elem, "v->ptr[i]") + "\n";
                }
                b += "    dp_free(v->ptr);\n";
                break;
            case TK::Map:
                if (ir::needs_drop(t->key) || ir::needs_drop(t->elem)) {
                    b += "    for (int64_t i = 0; i < v->len; i++) {\n" + drop_line(t->key, "v->keys[i]", "        ") +
                         drop_line(t->elem, "v->vals[i]", "        ") + "    }\n";
                }
                b += "    dp_free(v->keys);\n    dp_free(v->vals);\n    dp_free(v->slots);\n";
                break;
            case TK::Optional: b += "    if (v->has) {\n" + drop_line(t->elem, "v->value", "        ") + "    }\n"; break;
            case TK::Result:
                b += "    if (v->is_err) {\n" + drop_line(t->err, "v->err", "        ") + "    } else {\n";
                if (t->elem->kind != TK::Void) b += drop_line(t->elem, "v->value", "        ");
                b += "    }\n";
                break;
            case TK::Struct:
                for (const sema::FieldDef& f : t->sdef->fields) b += drop_line(f.type, "v->f_" + f.name, "    ");
                break;
            case TK::Enum:
                b += "    switch (v->tag) {\n";
                for (uint32_t vi = 0; vi < t->edef->variants.size(); vi++) {
                    const sema::VariantDef& vd = t->edef->variants[vi];
                    std::string body;
                    for (uint32_t k = 0; k < payload_count(vd); k++) {
                        body += drop_line(payload_type(t, vi, k), "v->u.v_" + vd.name + ".p" + std::to_string(k),
                                          "            ");
                    }
                    if (!body.empty()) b += "        case " + std::to_string(vi) + ":\n" + body + "            break;\n";
                }
                b += "        default: break;\n    }\n";
                break;
            default: break;
        }
        b += "    *v = (" + n + "){0};\n";
        return b + "}\n\n";
    }

    std::string copy_def(const Type* t) {
        std::string n = ctype(t), m = mangle(t);
        std::string b = "DP_INLINE " + n + " dp_copy__" + m + "(const " + n + "* v) {\n";
        switch (t->kind) {
            case TK::List: {
                std::string ec = ctype(t->elem);
                b += "    " + n + " r = {0};\n    r.ptr = (" + ec + "*)dp_alloc(v->len, sizeof(" + ec +
                     "), NULL);\n    r.len = v->len;\n    r.cap = v->len;\n";
                b += "    for (int64_t i = 0; i < v->len; i++) r.ptr[i] = " + copy_of(t->elem, "v->ptr[i]") + ";\n";
                break;
            }
            case TK::Map: {
                std::string kc = ctype(t->key), vc = ctype(t->elem);
                b += "    " + n + " r = {0};\n";
                b += "    r.keys = (" + kc + "*)dp_alloc(v->len, sizeof(" + kc + "), NULL);\n";
                b += "    r.vals = (" + vc + "*)dp_alloc(v->len, sizeof(" + vc + "), NULL);\n";
                b += "    r.len = v->len;\n    r.cap = v->len;\n    for (int64_t i = 0; i < v->len; i++) {\n";
                b += "        r.keys[i] = " + copy_of(t->key, "v->keys[i]") + ";\n";
                b += "        r.vals[i] = " + copy_of(t->elem, "v->vals[i]") + ";\n    }\n";
                b += "    if (v->nslots > 0) {\n        r.slots = (int64_t*)dp_alloc(v->nslots, sizeof(int64_t), NULL);\n";
                b += "        memcpy(r.slots, v->slots, (size_t)v->nslots * sizeof(int64_t));\n        r.nslots = v->nslots;\n    }\n";
                break;
            }
            case TK::Optional:
                b += "    " + n + " r = *v;\n    if (v->has) r.value = " + copy_of(t->elem, "v->value") + ";\n";
                break;
            case TK::Result:
                b += "    " + n + " r = *v;\n    if (v->is_err) r.err = " + copy_of(t->err, "v->err") + ";\n";
                if (t->elem->kind != TK::Void) b += "    else r.value = " + copy_of(t->elem, "v->value") + ";\n";
                break;
            case TK::Struct:
                b += "    " + n + " r = *v;\n";
                for (const sema::FieldDef& f : t->sdef->fields) {
                    if (ir::needs_drop(f.type)) b += "    r.f_" + f.name + " = " + copy_of(f.type, "v->f_" + f.name) + ";\n";
                }
                break;
            case TK::Enum:
                b += "    " + n + " r = *v;\n    switch (v->tag) {\n";
                for (uint32_t vi = 0; vi < t->edef->variants.size(); vi++) {
                    const sema::VariantDef& vd = t->edef->variants[vi];
                    std::string body;
                    for (uint32_t k = 0; k < payload_count(vd); k++) {
                        const Type* pt = payload_type(t, vi, k);
                        if (!ir::needs_drop(pt)) continue;
                        std::string fld = "u.v_" + vd.name + ".p" + std::to_string(k);
                        body += "            r." + fld + " = " + copy_of(pt, "v->" + fld) + ";\n";
                    }
                    if (!body.empty()) b += "        case " + std::to_string(vi) + ":\n" + body + "            break;\n";
                }
                b += "        default: break;\n    }\n";
                break;
            default: b += "    " + n + " r = *v;\n"; break;
        }
        return b + "    return r;\n}\n\n";
    }

    // Хэш ключа Map (Р54): целое и bool — перемешивание, str — FNV-1a (dp_rt.h).
    static std::string hash_of(const Type* k, const std::string& x) {
        if (k->kind == TK::Str) return "dp_hash_str(" + x + ")";
        return "dp_hash_u64((uint64_t)(" + x + "))";
    }

    // Методы Map (Р54). Поиск — открытая адресация по индексу slots
    // (заполнен не больше чем наполовину); порядок пар — порядок вставки.
    std::string map_methods(const Type* t) {
        std::string n = ctype(t), m = mangle(t), kc = ctype(t->key), vc = ctype(t->elem);
        std::string km = mangle(t->key), vm = mangle(t->elem);
        auto drop_stmt = [&](const Type* x, const std::string& lv) {
            std::string d = drop_of(x, lv);
            return d.empty() ? std::string() : "    " + d + "\n";
        };
        std::string b;
        b += "DP_INLINE int64_t dp_mfind__" + m + "(const " + n + "* m, const " + kc + "* key) {\n";
        b += "    if (m->nslots == 0) return -1;\n    uint64_t mask = (uint64_t)m->nslots - 1;\n";
        b += "    for (uint64_t i = " + hash_of(t->key, "*key") + " & mask;; i = (i + 1) & mask) {\n";
        b += "        int64_t s = m->slots[i];\n        if (s == 0) return -1;\n";
        b += "        if (" + eq_of(t->key, "&m->keys[s - 1]", "key") + ") return s - 1;\n    }\n}\n\n";
        // Индекс заново под len: размер — степень двойки не меньше 2 * len + 2.
        b += "DP_INLINE void dp_mindex__" + m + "(" + n + "* m, const char* loc) {\n";
        b += "    int64_t ns = 8;\n    while (ns < 2 * m->len + 2) ns *= 2;\n    dp_free(m->slots);\n";
        b += "    m->slots = (int64_t*)dp_alloc(ns, sizeof(int64_t), loc);\n";
        b += "    memset(m->slots, 0, (size_t)ns * sizeof(int64_t));\n    m->nslots = ns;\n";
        b += "    uint64_t mask = (uint64_t)ns - 1;\n    for (int64_t p = 0; p < m->len; p++) {\n";
        b += "        uint64_t i = " + hash_of(t->key, "m->keys[p]") + " & mask;\n";
        b += "        while (m->slots[i] != 0) i = (i + 1) & mask;\n        m->slots[i] = p + 1;\n    }\n}\n\n";
        // Вставка или замена: ключ и значение отдаются целиком.
        b += "DP_INLINE void dp_mset__" + m + "(" + n + "* m, " + kc + " key, " + vc + " val, const char* loc) {\n";
        b += "    int64_t p = dp_mfind__" + m + "(m, &key);\n    if (p >= 0) {\n";
        b += "    " + drop_stmt(t->key, "key") + "    " + drop_stmt(t->elem, "m->vals[p]");
        b += "        m->vals[p] = val;\n        return;\n    }\n";
        b += "    if (m->len == m->cap) {\n        int64_t nc = m->cap ? m->cap * 2 : 4;\n";
        b += "        if (nc > DP_MAX_BYTES / (int64_t)sizeof(" + kc + ") || nc > DP_MAX_BYTES / (int64_t)sizeof(" + vc +
             ")) nc = m->len + 1;\n";
        b += "        m->keys = (" + kc + "*)dp_realloc(m->keys, nc, sizeof(" + kc + "), loc);\n";
        b += "        m->vals = (" + vc + "*)dp_realloc(m->vals, nc, sizeof(" + vc + "), loc);\n        m->cap = nc;\n    }\n";
        b += "    m->keys[m->len] = key;\n    m->vals[m->len] = val;\n    m->len++;\n";
        b += "    if (2 * m->len + 2 > m->nslots) {\n        dp_mindex__" + m + "(m, loc);\n        return;\n    }\n";
        b += "    uint64_t mask = (uint64_t)m->nslots - 1;\n";
        b += "    uint64_t i = " + hash_of(t->key, "m->keys[m->len - 1]") + " & mask;\n";
        b += "    while (m->slots[i] != 0) i = (i + 1) & mask;\n    m->slots[i] = m->len;\n}\n\n";
        b += "DP_INLINE " + vc + "* dp_mvalue__" + m + "(" + n + "* m, const " + kc + "* key, const char* loc) {\n";
        b += "    int64_t p = dp_mfind__" + m + "(m, key);\n";
        b += "    if (p < 0) dp_trap(loc, \"key is not in the map\");\n    return &m->vals[p];\n}\n\n";
        b += "DP_INLINE " + kc + "* dp_mkey__" + m + "(" + n + "* m, int64_t i, const char* loc) {\n";
        b += "    return &m->keys[dp_check_index(i, m->len, loc)];\n}\n\n";
        b += "DP_INLINE bool dp_mcontains__" + m + "(const " + n + "* m, const " + kc + "* key) {\n";
        b += "    return dp_mfind__" + m + "(m, key) >= 0;\n}\n\n";
        b += "DP_INLINE void dp_mclear__" + m + "(" + n + "* m) {\n";
        if (ir::needs_drop(t->key) || ir::needs_drop(t->elem)) {
            b += "    for (int64_t i = 0; i < m->len; i++) {\n" + drop_stmt(t->key, "m->keys[i]") +
                 drop_stmt(t->elem, "m->vals[i]") + "    }\n";
        }
        b += "    m->len = 0;\n    if (m->nslots > 0) memset(m->slots, 0, (size_t)m->nslots * sizeof(int64_t));\n}\n\n";
        if (mangled_.count("O_" + vm)) {
            std::string o = "dp_O_" + vm;
            b += "DP_INLINE " + o + " dp_mget__" + m + "(const " + n + "* m, const " + kc + "* key) {\n";
            b += "    " + o + " r = {0};\n    int64_t p = dp_mfind__" + m + "(m, key);\n";
            b += "    if (p >= 0) {\n        r.has = true;\n        r.value = " + copy_of(t->elem, "m->vals[p]") +
                 ";\n    }\n    return r;\n}\n\n";
            // Удаление сохраняет порядок остальных пар: сдвиг и индекс заново — O(n).
            b += "DP_INLINE " + o + " dp_mremove__" + m + "(" + n + "* m, " + kc + " key, const char* loc) {\n";
            b += "    " + o + " r = {0};\n    int64_t p = dp_mfind__" + m + "(m, &key);\n" + drop_stmt(t->key, "key");
            b += "    if (p < 0) return r;\n    r.has = true;\n    r.value = m->vals[p];\n" + drop_stmt(t->key, "m->keys[p]");
            b += "    memmove(&m->keys[p], &m->keys[p + 1], (size_t)(m->len - 1 - p) * sizeof(" + kc + "));\n";
            b += "    memmove(&m->vals[p], &m->vals[p + 1], (size_t)(m->len - 1 - p) * sizeof(" + vc + "));\n";
            b += "    m->len--;\n    dp_mindex__" + m + "(m, loc);\n    return r;\n}\n\n";
        }
        if (mangled_.count("L_" + km)) {
            std::string l = "dp_L_" + km;
            b += "DP_INLINE " + l + " dp_mkeys__" + m + "(const " + n + "* m) {\n    " + l + " r = {0};\n";
            b += "    r.ptr = (" + kc + "*)dp_alloc(m->len, sizeof(" + kc + "), NULL);\n    r.len = m->len;\n    r.cap = m->len;\n";
            b += "    for (int64_t i = 0; i < m->len; i++) r.ptr[i] = " + copy_of(t->key, "m->keys[i]") + ";\n";
            b += "    return r;\n}\n\n";
        }
        return b;
    }

    std::string list_methods(const Type* t) {
        std::string n = ctype(t), m = mangle(t), ec = ctype(t->elem), e = mangle(t->elem);
        std::string b;
        b += "DP_INLINE void dp_push__" + m + "(" + n + "* l, " + ec + " v, const char* loc) {\n";
        b += "    if (l->len == l->cap) {\n        int64_t nc = l->cap ? l->cap * 2 : 4;\n";
        b += "        if (nc > DP_MAX_BYTES / (int64_t)sizeof(" + ec + ")) nc = l->len + 1;\n";
        b += "        l->ptr = (" + ec + "*)dp_realloc(l->ptr, nc, sizeof(" + ec + "), loc);\n        l->cap = nc;\n    }\n";
        b += "    l->ptr[l->len++] = v;\n}\n\n";
        if (mangled_.count("O_" + e)) {
            b += "DP_INLINE dp_O_" + e + " dp_pop__" + m + "(" + n + "* l) {\n    dp_O_" + e + " r = {0};\n";
            b += "    if (l->len > 0) {\n        r.has = true;\n        r.value = l->ptr[--l->len];\n    }\n    return r;\n}\n\n";
        }
        b += "DP_INLINE void dp_insert__" + m + "(" + n + "* l, int64_t i, " + ec + " v, const char* loc) {\n";
        b += "    dp_check_insert(i, l->len, loc);\n    dp_push__" + m + "(l, v, loc);\n";
        b += "    memmove(&l->ptr[i + 1], &l->ptr[i], (size_t)(l->len - 1 - i) * sizeof(" + ec + "));\n";
        b += "    l->ptr[i] = v;\n}\n\n";
        b += "DP_INLINE " + ec + " dp_remove__" + m + "(" + n + "* l, int64_t i, const char* loc) {\n";
        b += "    dp_check_index(i, l->len, loc);\n    " + ec + " v = l->ptr[i];\n";
        b += "    memmove(&l->ptr[i], &l->ptr[i + 1], (size_t)(l->len - 1 - i) * sizeof(" + ec + "));\n";
        b += "    l->len--;\n    return v;\n}\n\n";
        b += "DP_INLINE void dp_clear__" + m + "(" + n + "* l) {\n";
        if (ir::needs_drop(t->elem)) b += "    for (int64_t i = 0; i < l->len; i++) " + drop_of(t->elem, "l->ptr[i]") + "\n";
        b += "    l->len = 0;\n}\n\n";
        // Сортировка слиянием снизу вверх — та же, что у интерпретатора:
        // при любом сравнении (и с NaN) даёт ту же перестановку.
        b += "DP_INLINE void dp_sort__" + m + "(" + n + "* l) {\n    int64_t n = l->len;\n    if (n < 2) return;\n";
        b += "    " + ec + "* a = l->ptr;\n    " + ec + "* b = (" + ec + "*)dp_alloc(n, sizeof(" + ec + "), NULL);\n";
        b += "    for (int64_t w = 1; w < n; w *= 2) {\n        for (int64_t lo = 0; lo < n; lo += 2 * w) {\n";
        b += "            int64_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;\n";
        b += "            int64_t i = lo, j = mid, k = lo;\n";
        b += "            while (i < mid && j < hi) {\n                if (" + lt_of(t->elem, "&a[j]", "&a[i]") +
             ") b[k++] = a[j++];\n                else b[k++] = a[i++];\n            }\n";
        b += "            while (i < mid) b[k++] = a[i++];\n            while (j < hi) b[k++] = a[j++];\n        }\n";
        b += "        " + ec + "* t = a;\n        a = b;\n        b = t;\n    }\n";
        b += "    if (a != l->ptr) {\n        dp_free(l->ptr);\n        l->ptr = a;\n        l->cap = n;\n    } else {\n";
        b += "        dp_free(b);\n    }\n}\n\n";
        b += "DP_INLINE " + n + " dp_repeat__" + m + "(const " + ec + "* x, int64_t n, const char* loc) {\n";
        b += "    " + n + " r = {0};\n    r.ptr = (" + ec + "*)dp_alloc(n, sizeof(" + ec + "), loc);\n";
        b += "    r.len = n;\n    r.cap = n;\n";
        b += "    for (int64_t i = 0; i < n; i++) r.ptr[i] = " + copy_of(t->elem, "*x") + ";\n    return r;\n}\n\n";
        if (t->elem->kind == TK::Str) {
            b += "DP_INLINE " + n + " dp_split__" + m + "(dp_str s, dp_str sep, const char* loc) {\n";
            b += "    if (sep.len == 0) dp_trap(loc, \"split separator is empty\");\n";
            b += "    " + n + " out = {0};\n    int64_t a = 0, p = 0;\n    for (;;) {\n";
            b += "        if (!dp_str_find_from(s, sep, a, &p)) {\n            dp_push__" + m +
                 "(&out, dp_str_sub(s, a, s.len), loc);\n            return out;\n        }\n";
            b += "        dp_push__" + m + "(&out, dp_str_sub(s, a, p), loc);\n        a = p + sep.len;\n    }\n}\n\n";
        }
        return b;
    }

    // --- функции программы

    std::string function(const Function& f);
    std::string wrapper(const Function& f, const std::string& name);
    std::string uses();
    std::string entry();
    std::string test_entry();
};

// Перевод одной функции: инструкции — блоками C со своими временными.
class FnEmitter {
public:
    FnEmitter(Emitter& em, const ir::Program& p, const Function& f, const SourceMap& src,
              std::vector<std::string>& errors)
        : em_(em), p_(p), f_(f), src_(src), errors_(errors) {}

    std::string run();

private:
    Emitter& em_;
    const ir::Program& p_;
    const Function& f_;
    const SourceMap& src_;
    std::vector<std::string>& errors_;
    uint32_t tmp_ = 0;
    std::string pre_;   // инструкции C до значения
    std::string post_;  // освобождение забранных операндов после
    std::string loc_;   // место текущей инструкции — строкой C

    void error(const std::string& msg) { errors_.push_back(f_.name + ": " + msg); }
    std::string fresh(const char* base) { return std::string(base) + std::to_string(tmp_++); }
    void line(const std::string& s) { pre_ += "        " + s + "\n"; }

    std::string local_expr(uint32_t i) {
        std::string n = "l" + std::to_string(i) + (f_.locals[i].name.empty() ? "" : "_" + f_.locals[i].name);
        const ir::Local& l = f_.locals[i];
        return l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut ? "(*" + n + ")" : n;
    }

    // Место — выражение C; байт строки — значение во временном (byte = true).
    struct Lv {
        std::string expr;
        const Type* type = nullptr;
        bool byte = false;
    };

    Lv place(const Place& pl) {
        Lv r{local_expr(pl.local), f_.locals[pl.local].type, false};
        for (const ir::Proj& pr : pl.proj) {
            const Type* t = r.type;
            if (r.byte) {
                error("projection of a str byte");
                return r;
            }
            switch (pr.kind) {
                case ProjKind::Field: {
                    const sema::FieldDef& fd = t->sdef->fields[pr.index];
                    r.expr = "(" + r.expr + ").f_" + fd.name;
                    r.type = fd.type;
                    break;
                }
                case ProjKind::Index: {
                    std::string idx = local_expr(pr.index);
                    if (t->kind == TK::Str) {
                        std::string b = fresh("dp_b");
                        line("uint8_t " + b + " = dp_str_at(" + r.expr + ", " + idx + ", " + loc_ + ");");
                        r.expr = b;
                        r.type = p_.types->prim(TK::U8);
                        r.byte = true;
                        break;
                    }
                    std::string ptr = fresh("dp_p");
                    line(ctype(t->elem) + "* " + ptr + " = &(" + r.expr + ").ptr[dp_check_index(" + idx + ", (" +
                         r.expr + ").len, " + loc_ + ")];");
                    r.expr = "(*" + ptr + ")";
                    r.type = t->elem;
                    break;
                }
                case ProjKind::Payload: {
                    const sema::VariantDef& vd = t->edef->variants[pr.index];
                    r.expr = "(" + r.expr + ").u.v_" + vd.name + ".p" + std::to_string(pr.sub);
                    r.type = payload_type(t, pr.index, pr.sub);
                    break;
                }
                case ProjKind::Unwrap: r.expr = "(" + r.expr + ").value"; r.type = t->elem; break;
                case ProjKind::ResultValue: r.expr = "(" + r.expr + ").value"; r.type = t->elem; break;
                case ProjKind::ResultError: r.expr = "(" + r.expr + ").err"; r.type = t->err; break;
                case ProjKind::MapKey: case ProjKind::MapValue: {  // Р54
                    bool key = pr.kind == ProjKind::MapKey;
                    const Type* et = key ? t->key : t->elem;
                    std::string ptr = fresh("dp_p");
                    std::string arg = key ? local_expr(pr.index) : "&(" + local_expr(pr.index) + ")";
                    line(ctype(et) + "* " + ptr + " = dp_m" + (key ? "key" : "value") + "__" + mangle(t) + "(&(" +
                         r.expr + "), " + arg + ", " + loc_ + ");");
                    r.expr = "(*" + ptr + ")";
                    r.type = et;
                    break;
                }
            }
        }
        return r;
    }

    std::string constant(const Const& c, bool store) {
        const Type* t = c.type;
        switch (c.kind) {
            case Const::Kind::Bool: return c.b ? "true" : "false";
            case Const::Kind::Str: {
                const std::string& s = p_.strings[c.str];
                std::string n = "dp_s" + std::to_string(c.str);
                std::string len = std::to_string(s.size());
                return store ? "dp_str_own(" + n + ", " + len + ", NULL)" : "DP_STR_LIT(" + n + ", " + len + ")";
            }
            case Const::Kind::Float: {
                std::string v;
                if (std::isnan(c.f)) v = "NAN";
                else if (std::isinf(c.f)) v = c.f < 0 ? "(-INFINITY)" : "INFINITY";
                else {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%a", c.f);
                    v = buf;
                }
                return "((" + ctype(t) + ")" + v + ")";
            }
            case Const::Kind::Int: {
                if (sema::is_signed(t)) {
                    int64_t v = static_cast<int64_t>(c.i);
                    std::string lit = v == INT64_MIN ? "INT64_MIN" : "INT64_C(" + std::to_string(v) + ")";
                    return "((" + ctype(t) + ")" + lit + ")";
                }
                return "((" + ctype(t) + ")UINT64_C(" + std::to_string(c.i) + "))";
            }
        }
        return "0";
    }

    // Операнд во временном. store — значение сохраняется (копия глубокая,
    // move забирает); иначе только читается: забранное освобождается после.
    std::string operand(const Operand& o, bool store) {
        if (o.kind == OpKind::Const) return constant(o.c, store);
        Lv lv = place(o.place);
        std::string v = fresh("dp_v");
        if (lv.byte) {
            line("uint8_t " + v + " = " + lv.expr + ";");
            return v;
        }
        std::string ct = ctype(lv.type);
        if (o.kind == OpKind::Move && ir::needs_drop(lv.type)) {
            line(ct + " " + v + " = " + lv.expr + ";");
            line(lv.expr + " = (" + ct + "){0};");
            if (!store) post_ += "        " + drop_of(lv.type, v) + "\n";
            return v;
        }
        line(ct + " " + v + " = " + (store ? copy_of(lv.type, lv.expr) : lv.expr) + ";");
        return v;
    }

    const Type* op_type(const Operand& o) { return ir::operand_type(p_, f_, o); }

    std::string tname(const Type* t) { return "\"" + sema::type_name(t) + "\""; }
    std::string bits(const Type* t) { return std::to_string(sema::int_bits(t)); }

    std::string rvalue(const Rvalue& rv);
    std::string binary(const Rvalue& rv);
    std::string unary(const Rvalue& rv);
    std::string cast(const Rvalue& rv);
    std::string intrinsic(const Rvalue& rv);
    std::string inst(const Inst& in);
    std::string term(const Term& t);
};

std::string FnEmitter::binary(const Rvalue& rv) {
    std::string a = operand(rv.ops[0], false);
    std::string b = operand(rv.ops[1], false);
    const Type* t = op_type(rv.ops[0]);
    std::string ct = ctype(t);
    switch (rv.bin) {
        case BinKind::Concat: return "dp_str_concat(" + a + ", " + b + ", " + loc_ + ")";
        case BinKind::Eq: case BinKind::Ne: case BinKind::Lt: case BinKind::Le: case BinKind::Gt: case BinKind::Ge: {
            if (is_scalar(t)) {
                const char* op = rv.bin == BinKind::Eq ? "==" : rv.bin == BinKind::Ne ? "!=" : rv.bin == BinKind::Lt ? "<"
                                 : rv.bin == BinKind::Le ? "<=" : rv.bin == BinKind::Gt ? ">" : ">=";
                return "(" + a + " " + op + " " + b + ")";
            }
            // Константа str — значение, а не место: адрес нужен временному.
            std::string pa = fresh("dp_c"), pb = fresh("dp_c");
            line(ct + " " + pa + " = " + a + ";");
            line(ct + " " + pb + " = " + b + ";");
            std::string A = "&" + pa, B = "&" + pb;
            switch (rv.bin) {
                case BinKind::Eq: return eq_of(t, A, B);
                case BinKind::Ne: return "(!" + eq_of(t, A, B) + ")";
                case BinKind::Lt: return lt_of(t, A, B);
                case BinKind::Gt: return lt_of(t, B, A);
                case BinKind::Le: return "(!" + lt_of(t, B, A) + " && (" + eq_of(t, A, B) + " || " + lt_of(t, A, B) + "))";
                default: return "(!" + lt_of(t, A, B) + " && (" + eq_of(t, A, B) + " || " + lt_of(t, B, A) + "))";
            }
        }
        default: break;
    }
    if (sema::is_float(t)) {
        const char* op = rv.bin == BinKind::Add ? "+" : rv.bin == BinKind::Sub ? "-" : rv.bin == BinKind::Mul ? "*"
                         : rv.bin == BinKind::Div ? "/" : nullptr;
        if (!op) {
            error("integer operation on floats");
            return "0";
        }
        return "((" + ct + ")(" + a + " " + op + " " + b + "))";
    }
    bool sg = sema::is_signed(t);
    std::string B = bits(t), N = tname(t);
    std::string sa = "(int64_t)" + a, sb = "(int64_t)" + b, ua = "(uint64_t)" + a, ub = "(uint64_t)" + b;
    auto chk = [&](const char* fn) {
        if (sg) return "((" + ct + ")dp_s" + fn + "(" + sa + ", " + sb + ", " + B + ", " + N + ", " + loc_ + "))";
        return "((" + ct + ")dp_u" + fn + "(" + ua + ", " + ub + ", " + B + ", " + N + ", " + loc_ + "))";
    };
    auto bitsop = [&](const std::string& expr) {
        return sg ? "((" + ct + ")dp_sbits(" + expr + ", " + B + "))" : "((" + ct + ")dp_ubits(" + expr + ", " + B + "))";
    };
    switch (rv.bin) {
        case BinKind::Add: return chk("add");
        case BinKind::Sub: return chk("sub");
        case BinKind::Mul: return chk("mul");
        case BinKind::Div:
            if (sg) return "((" + ct + ")dp_sdiv(" + sa + ", " + sb + ", " + B + ", " + N + ", " + loc_ + "))";
            return "((" + ct + ")dp_udiv(" + ua + ", " + ub + ", " + loc_ + "))";
        case BinKind::Rem:
            if (sg) return "((" + ct + ")dp_srem(" + sa + ", " + sb + ", " + B + ", " + N + ", " + loc_ + "))";
            return "((" + ct + ")dp_urem(" + ua + ", " + ub + ", " + loc_ + "))";
        case BinKind::AddWrap: return bitsop(ua + " + " + ub);
        case BinKind::SubWrap: return bitsop(ua + " - " + ub);
        case BinKind::MulWrap: return bitsop(ua + " * " + ub);
        case BinKind::BitAnd: return bitsop(ua + " & " + ub);
        case BinKind::BitOr: return bitsop(ua + " | " + ub);
        case BinKind::BitXor: return bitsop(ua + " ^ " + ub);
        case BinKind::Shl: case BinKind::Shr: {
            const Type* bt = op_type(rv.ops[1]);
            std::string n = fresh("dp_n");
            if (sema::is_signed(bt)) line("uint64_t " + n + " = dp_shift_count_s(" + sb + ", " + B + ", " + N + ", " + loc_ + ");");
            else line("uint64_t " + n + " = dp_shift_count_u(" + ub + ", " + B + ", " + N + ", " + loc_ + ");");
            if (rv.bin == BinKind::Shl) {
                if (sg) return "((" + ct + ")dp_sshl(" + sa + ", " + n + ", " + B + "))";
                return "((" + ct + ")dp_ushl(" + ua + ", " + n + ", " + B + "))";
            }
            if (sg) return "((" + ct + ")dp_sshr(" + sa + ", " + n + "))";
            return "((" + ct + ")(" + ua + " >> " + n + "))";
        }
        default: break;
    }
    error("unknown binary operation");
    return "0";
}

std::string FnEmitter::unary(const Rvalue& rv) {
    std::string a = operand(rv.ops[0], false);
    const Type* t = rv.type;
    std::string ct = ctype(t);
    switch (rv.un) {
        case ir::UnKind::Not: return "(!" + a + ")";
        case ir::UnKind::BitNot:
            if (sema::is_signed(t)) return "((" + ct + ")dp_sbits(~(uint64_t)" + a + ", " + bits(t) + "))";
            return "((" + ct + ")dp_ubits(~(uint64_t)" + a + ", " + bits(t) + "))";
        case ir::UnKind::Neg:
            if (sema::is_float(t)) return "((" + ct + ")(-" + a + "))";
            return "((" + ct + ")dp_sneg((int64_t)" + a + ", " + bits(t) + ", " + tname(t) + ", " + loc_ + "))";
    }
    return "0";
}

std::string FnEmitter::cast(const Rvalue& rv) {
    std::string a = operand(rv.ops[0], false);
    const Type* from = op_type(rv.ops[0]);
    const Type* to = rv.type;
    std::string ct = ctype(to);
    switch (rv.cast) {
        case CastKind::Widen: return "((" + ct + ")" + a + ")";
        case CastKind::Wrap:
            if (sema::is_signed(to)) return "((" + ct + ")dp_sbits((uint64_t)" + a + ", " + bits(to) + "))";
            return "((" + ct + ")dp_ubits((uint64_t)" + a + ", " + bits(to) + "))";
        case CastKind::ToStr:
            if (from->kind == TK::Bool) return "dp_str_from_bool(" + a + ")";
            if (sema::is_float(from)) {
                return "dp_str_from_f((double)" + a + ", " + (from->kind == TK::F32 ? "true" : "false") + ")";
            }
            if (sema::is_signed(from)) return "dp_str_from_s((int64_t)" + a + ")";
            return "dp_str_from_u((uint64_t)" + a + ")";
        case CastKind::Check: break;
    }
    std::string to_name = tname(to);
    if (sema::is_integer(from) && sema::is_integer(to)) {
        const char* fn = sema::is_signed(from) ? (sema::is_signed(to) ? "dp_cast_s_to_s" : "dp_cast_s_to_u")
                                               : (sema::is_signed(to) ? "dp_cast_u_to_s" : "dp_cast_u_to_u");
        std::string arg = sema::is_signed(from) ? "(int64_t)" + a : "(uint64_t)" + a;
        return "((" + ct + ")" + fn + "(" + arg + ", " + bits(to) + ", " + to_name + ", " + loc_ + "))";
    }
    if (sema::is_integer(from)) return "((" + ct + ")" + a + ")";  // одно округление к ближайшему
    if (sema::is_float(to)) {
        if (to->kind == TK::F32 && from->kind == TK::F64) return "dp_cast_f64_to_f32(" + a + ", " + loc_ + ")";
        return "((" + ct + ")" + a + ")";
    }
    std::string f32 = from->kind == TK::F32 ? "true" : "false";
    if (sema::is_signed(to)) {
        return "((" + ct + ")dp_cast_f_to_s((double)" + a + ", " + f32 + ", " + bits(to) + ", " + to_name + ", " + loc_ + "))";
    }
    return "((" + ct + ")dp_cast_f_to_u((double)" + a + ", " + f32 + ", " + bits(to) + ", " + to_name + ", " + loc_ + "))";
}

std::string FnEmitter::intrinsic(const Rvalue& rv) {
    if (rv.has_place && rv.intrinsic == Intrinsic::ReadFile) {
        // __read_file (Р51): путь читается, текст — в место; результат — код.
        std::string path = operand(rv.ops[0], false);
        Lv out = place(rv.place);
        return "dp_read_file(" + path + ", &" + out.expr + ", " + loc_ + ")";
    }
    if (rv.has_place) {
        // Метод List: операнды — копии, вычисленные до места списка.
        std::vector<std::string> ops;
        for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
        Lv lst = place(rv.place);
        std::string m = mangle(lst.type);
        std::string l = "&" + lst.expr;
        switch (rv.intrinsic) {
            case Intrinsic::ListPush: return "dp_push__" + m + "(" + l + ", " + ops[0] + ", " + loc_ + ")";
            case Intrinsic::ListPop: return "dp_pop__" + m + "(" + l + ")";
            case Intrinsic::ListInsert:
                return "dp_insert__" + m + "(" + l + ", (int64_t)" + ops[0] + ", " + ops[1] + ", " + loc_ + ")";
            case Intrinsic::ListRemove: return "dp_remove__" + m + "(" + l + ", (int64_t)" + ops[0] + ", " + loc_ + ")";
            case Intrinsic::ListClear: return "dp_clear__" + m + "(" + l + ")";
            case Intrinsic::ListSort: return "dp_sort__" + m + "(" + l + ")";
            case Intrinsic::MapSet: return "dp_mset__" + m + "(" + l + ", " + ops[0] + ", " + ops[1] + ", " + loc_ + ")";
            case Intrinsic::MapRemove: return "dp_mremove__" + m + "(" + l + ", " + ops[0] + ", " + loc_ + ")";
            case Intrinsic::MapClear: return "dp_mclear__" + m + "(" + l + ")";
            default: error("unknown list method");
                return "0";
        }
    }
    std::vector<std::string> ops;
    for (const Operand& o : rv.ops) ops.push_back(operand(o, false));
    switch (rv.intrinsic) {
        case Intrinsic::Len: return "((int64_t)(" + ops[0] + ").len)";
        case Intrinsic::Repeat: {
            const Type* nt = op_type(rv.ops[1]);
            std::string n = fresh("dp_n");
            if (sema::is_signed(nt)) {
                line("dp_check_repeat((int64_t)" + ops[1] + ", " + loc_ + ");");
                line("int64_t " + n + " = (int64_t)" + ops[1] + ";");
            } else {
                line("if ((uint64_t)" + ops[1] + " > (uint64_t)INT64_MAX) dp_trap(" + loc_ + ", \"out of memory\");");
                line("int64_t " + n + " = (int64_t)" + ops[1] + ";");
            }
            std::string x = fresh("dp_x");
            line(ctype(op_type(rv.ops[0])) + " " + x + " = " + ops[0] + ";");
            return "dp_repeat__" + mangle(rv.type) + "(&" + x + ", " + n + ", " + loc_ + ")";
        }
        case Intrinsic::Min: return "(" + ops[0] + " < " + ops[1] + " ? " + ops[0] + " : " + ops[1] + ")";
        case Intrinsic::Max: return "(" + ops[0] + " < " + ops[1] + " ? " + ops[1] + " : " + ops[0] + ")";
        case Intrinsic::Abs: {
            const Type* t = rv.type;
            if (sema::is_float(t)) return "((" + ctype(t) + ")fabs((double)" + ops[0] + "))";
            return "((" + ctype(t) + ")dp_sabs((int64_t)" + ops[0] + ", " + bits(t) + ", " + tname(t) + ", " + loc_ + "))";
        }
        case Intrinsic::CheckedAdd: case Intrinsic::CheckedSub: case Intrinsic::CheckedMul:
        case Intrinsic::CheckedDiv: case Intrinsic::CheckedNeg: {
            const Type* t = rv.type->elem;
            bool sg = sema::is_signed(t);
            const char* fn = rv.intrinsic == Intrinsic::CheckedAdd ? "add"
                             : rv.intrinsic == Intrinsic::CheckedSub ? "sub"
                             : rv.intrinsic == Intrinsic::CheckedMul ? "mul"
                             : rv.intrinsic == Intrinsic::CheckedDiv ? "div" : "neg";
            std::string cast = sg ? "(int64_t)" : "(uint64_t)";
            std::string r = fresh("dp_r"), v = fresh("dp_v");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(std::string(sg ? "int64_t " : "uint64_t ") + v + " = 0;");
            std::string args = cast + ops[0] + (rv.intrinsic == Intrinsic::CheckedNeg ? "" : ", " + cast + ops[1]);
            line(r + ".has = dp_try_" + (sg ? "s" : "u") + fn + "(" + args + ", " + bits(t) + ", &" + v + ");");
            line("if (" + r + ".has) " + r + ".value = (" + ctype(t) + ")" + v + ";");
            return r;
        }
        case Intrinsic::ParseInt: {
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".has = dp_parse_int(" + ops[0] + ", &" + r + ".value);");
            return r;
        }
        case Intrinsic::IsDigit: return "dp_is_digit((uint64_t)" + ops[0] + ")";
        case Intrinsic::IsAlpha: return "dp_is_alpha((uint64_t)" + ops[0] + ")";
        case Intrinsic::IsSpace: return "dp_is_space((uint64_t)" + ops[0] + ")";
        case Intrinsic::IsAlnum: return "dp_is_alnum((uint64_t)" + ops[0] + ")";
        case Intrinsic::Print: return "dp_print(" + ops[0] + ")";
        case Intrinsic::Println: return "dp_println(" + ops[0] + ")";
        case Intrinsic::Eprintln: return "dp_eprintln(" + ops[0] + ")";
        case Intrinsic::ListContains: {
            const Type* et = op_type(rv.ops[0])->elem;
            std::string x = fresh("dp_x");
            line(ctype(et) + " " + x + " = " + ops[1] + ";");
            return "dp_contains__" + mangle(et) + "((" + ops[0] + ").ptr, (" + ops[0] + ").len, &" + x + ")";
        }
        case Intrinsic::MapGet: case Intrinsic::MapContains: {  // Р54: ключ — во временном, по адресу
            const Type* mt = op_type(rv.ops[0]);
            std::string x = fresh("dp_x");
            line(ctype(mt->key) + " " + x + " = " + ops[1] + ";");
            const char* fn = rv.intrinsic == Intrinsic::MapGet ? "dp_mget__" : "dp_mcontains__";
            return fn + mangle(mt) + "(&" + ops[0] + ", &" + x + ")";
        }
        case Intrinsic::MapKeys: return "dp_mkeys__" + mangle(op_type(rv.ops[0])) + "(&" + ops[0] + ")";
        case Intrinsic::StrTrim: return "dp_str_trim(" + ops[0] + ")";
        case Intrinsic::StrFind: {
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".has = dp_str_find_from(" + ops[0] + ", " + ops[1] + ", 0, &" + r + ".value);");
            return r;
        }
        case Intrinsic::StrStartsWith: return "dp_str_starts_with(" + ops[0] + ", " + ops[1] + ")";
        case Intrinsic::StrSplit: return "dp_split__" + mangle(rv.type) + "(" + ops[0] + ", " + ops[1] + ", " + loc_ + ")";
        case Intrinsic::ArgsLen: return "dp_args_len()";
        case Intrinsic::ArgAt: return "dp_arg((int64_t)" + ops[0] + ", " + loc_ + ")";
        default: break;
    }
    error("unknown intrinsic");
    return "0";
}

std::string FnEmitter::rvalue(const Rvalue& rv) {
    switch (rv.kind) {
        case RvKind::Use: return operand(rv.ops[0], true);
        case RvKind::Binary: return binary(rv);
        case RvKind::Unary: return unary(rv);
        case RvKind::Cast: return cast(rv);
        case RvKind::MakeStruct: {
            std::vector<std::string> ops;
            for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            const sema::StructDef* sd = rv.type->sdef;
            for (size_t k = 0; k < ops.size() && k < sd->fields.size(); k++) {
                line(r + ".f_" + sd->fields[k].name + " = " + ops[k] + ";");
            }
            return r;
        }
        case RvKind::MakeVariant: {
            std::vector<std::string> ops;
            for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".tag = " + std::to_string(rv.variant) + ";");
            const sema::VariantDef& vd = rv.type->edef->variants[rv.variant];
            for (size_t k = 0; k < ops.size(); k++) {
                line(r + ".u.v_" + vd.name + ".p" + std::to_string(k) + " = " + ops[k] + ";");
            }
            return r;
        }
        case RvKind::MakeMap: return "((" + ctype(rv.type) + "){0})";
        case RvKind::MakeList: {
            std::vector<std::string> ops;
            for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
            std::string r = fresh("dp_r"), ec = ctype(rv.type->elem);
            line(ctype(rv.type) + " " + r + " = {0};");
            if (!ops.empty()) {
                std::string n = std::to_string(ops.size());
                line(r + ".ptr = (" + ec + "*)dp_alloc(" + n + ", sizeof(" + ec + "), " + loc_ + ");");
                line(r + ".len = " + n + ";");
                line(r + ".cap = " + n + ";");
                for (size_t k = 0; k < ops.size(); k++) line(r + ".ptr[" + std::to_string(k) + "] = " + ops[k] + ";");
            }
            return r;
        }
        case RvKind::MakeSome: {
            std::string v = operand(rv.ops[0], true);
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".has = true;");
            line(r + ".value = " + v + ";");
            return r;
        }
        case RvKind::MakeNone: return "((" + ctype(rv.type) + "){0})";
        case RvKind::MakeOk: case RvKind::MakeErr: {
            std::string v = rv.ops.empty() ? "" : operand(rv.ops[0], true);
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            if (rv.kind == RvKind::MakeErr) {
                line(r + ".is_err = true;");
                line(r + ".err = " + v + ";");
            } else if (!v.empty()) {
                line(r + ".value = " + v + ";");
            }
            return r;
        }
        case RvKind::SliceOf: {
            std::string lo = operand(rv.ops[0], false);
            std::string hi = rv.ops.size() > 1 ? operand(rv.ops[1], false) : "";
            Lv base = place(rv.place);
            std::string h = hi.empty() ? "(" + base.expr + ").len" : "(int64_t)" + hi;
            if (base.type->kind == TK::Str) return "dp_str_slice(" + base.expr + ", (int64_t)" + lo + ", " + h + ", " + loc_ + ")";
            std::string r = fresh("dp_r");
            line("dp_check_slice((int64_t)" + lo + ", " + h + ", (" + base.expr + ").len, " + loc_ + ");");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".ptr = (" + base.expr + ").ptr + (int64_t)" + lo + ";");
            line(r + ".len = " + h + " - (int64_t)" + lo + ";");
            return r;
        }
        case RvKind::Call: {
            line("DP_CALL_CHECK(" + loc_ + ");");
            const Function& g = p_.fns[rv.fn];
            std::vector<std::string> args;
            for (size_t i = 0; i < rv.args.size(); i++) {
                const Arg& a = rv.args[i];
                if (a.kind == Arg::Kind::Value) {
                    args.push_back(operand(a.op, true));
                    continue;
                }
                Lv lv = place(a.place);
                if (lv.byte) {
                    if (a.kind == Arg::Kind::Mut) error("&mut of a str byte");
                    args.push_back("&" + lv.expr);
                    continue;
                }
                std::string ptr = fresh("dp_a");
                line(ctype(lv.type) + "* " + ptr + " = &" + lv.expr + ";");
                args.push_back(ptr);
            }
            std::string call = Emitter::fn_name(g) + "(";
            for (size_t i = 0; i < args.size(); i++) call += (i ? ", " : "") + args[i];
            return call + ")";
        }
        case RvKind::Intrinsic: return intrinsic(rv);
        case RvKind::Tag: return "((uint32_t)(" + place(rv.place).expr + ").tag)";
        case RvKind::IsSome: return "((" + place(rv.place).expr + ").has)";
        case RvKind::IsOk: return "(!(" + place(rv.place).expr + ").is_err)";
    }
    error("unknown rvalue");
    return "0";
}

std::string FnEmitter::inst(const Inst& in) {
    pre_.clear();
    post_.clear();
    loc_ = em_.loc(in.span);
    std::string body;
    if (in.kind == InstKind::Drop) {
        Lv lv = place(in.dst);
        std::string d = drop_of(lv.type, lv.expr);
        if (!d.empty()) line(d);
    } else {
        std::string v = rvalue(in.rv);
        const Type* t = in.rv.type;
        bool is_void = !t || t->kind == TK::Void;
        if (in.kind == InstKind::Eval) {
            if (is_void) {
                line(v + ";");
            } else {
                std::string r = fresh("dp_e");
                line(ctype(t) + " " + r + " = " + v + ";");
                std::string d = drop_of(t, r);
                line(d.empty() ? "(void)" + r + ";" : d);
            }
        } else {
            std::string r = fresh("dp_e");
            Lv dst;
            if (is_void) {
                error("assignment of a void value");
            } else {
                line(ctype(t) + " " + r + " = " + v + ";");
            }
            dst = place(in.dst);
            std::string d = drop_of(dst.type, dst.expr);
            if (!d.empty()) line(d);
            line(dst.expr + " = " + r + ";");
        }
    }
    return em_.line_directive(in.span) + "    {\n" + pre_ + post_ + "    }\n";
}

std::string FnEmitter::term(const Term& t) {
    pre_.clear();
    post_.clear();
    loc_ = em_.loc(t.span);
    switch (t.kind) {
        case TermKind::Jump: line("goto bb" + std::to_string(t.then_b) + ";"); break;
        case TermKind::Branch: {
            std::string c = operand(t.cond, false);
            line("if (" + c + ") goto bb" + std::to_string(t.then_b) + ";");
            line("goto bb" + std::to_string(t.else_b) + ";");
            break;
        }
        case TermKind::Return:
            if (t.has_value) {
                std::string v = operand(t.value, true);
                line("dp_depth--;");
                line("return " + v + ";");
            } else {
                line("dp_depth--;");
                line("return;");
            }
            break;
        case TermKind::Trap: line("dp_trap(" + loc_ + ", " + cstr(t.message) + ");"); break;
        case TermKind::Unreachable: line("dp_unreachable(" + cstr(f_.name) + ");"); break;
        case TermKind::None: error("block without a terminator"); break;
    }
    return em_.line_directive(t.span) + "    {\n" + pre_ + "    }\n";
}

std::string FnEmitter::run() {
    std::string out = em_.fn_head(f_) + " {\n";
    for (uint32_t i = f_.param_count; i < f_.locals.size(); i++) {
        const Type* t = f_.locals[i].type;
        std::string n = "l" + std::to_string(i) + (f_.locals[i].name.empty() ? "" : "_" + f_.locals[i].name);
        out += "    " + ctype(t) + " " + n + (is_scalar(t) ? " = 0;\n" : " = {0};\n");
    }
    for (uint32_t i = 0; i < f_.locals.size(); i++) {
        out += "    (void)l" + std::to_string(i) + (f_.locals[i].name.empty() ? "" : "_" + f_.locals[i].name) + ";\n";
    }
    out += "    dp_depth++;\n";
    std::set<uint32_t> targets;
    for (const ir::Block& b : f_.blocks) {
        if (b.term.kind == TermKind::Jump) targets.insert(b.term.then_b);
        if (b.term.kind == TermKind::Branch) {
            targets.insert(b.term.then_b);
            targets.insert(b.term.else_b);
        }
    }
    for (uint32_t bi = 0; bi < f_.blocks.size(); bi++) {
        if (targets.count(bi)) out += "bb" + std::to_string(bi) + ":;\n";
        for (const Inst& in : f_.blocks[bi].insts) out += inst(in);
        out += term(f_.blocks[bi].term);
    }
    return out + "}\n\n";
}

std::string Emitter::function(const Function& f) {
    FnEmitter fe(*this, p_, f, src_, errors_);
    return fe.run();
}

// Обёртка функции без параметров (main или тест) для dp_run: вызывает её
// и возвращает имя варианта ошибки «E.V» или NULL.
std::string Emitter::wrapper(const Function& f, const std::string& name) {
    std::string out;
    const Type* ret = f.ret;
    if (ret->kind == TK::Result) {
        const Type* e = ret->err;
        out += "static const char* const " + name + "_err[] = {";
        for (const sema::VariantDef& v : e->edef->variants) out += cstr(e->edef->name + "." + v.name) + ", ";
        out += "NULL};\n\n";
        out += "static const char* " + name + "(void) {\n    " + ctype(ret) + " r = " + fn_name(f) + "();\n";
        out += "    const char* e = NULL;\n    if (r.is_err) e = " + name + "_err[r.err.tag];\n";
        std::string d = drop_of(ret, "r");
        if (!d.empty()) out += "    " + d + "\n";
        out += "    return e;\n}\n\n";
    } else {
        out += "static const char* " + name + "(void) {\n    " + fn_name(f) + "();\n    return NULL;\n}\n\n";
    }
    return out;
}

std::string Emitter::uses() {
    std::string out;
    for (size_t i = 0; i < p_.strings.size(); i++) out += "    (void)dp_s" + std::to_string(i) + ";\n";
    return out;
}

std::string Emitter::entry() {
    if (p_.main_fn < 0) {
        error("the program has no main function");
        return "";
    }
    std::string out = wrapper(p_.fns[static_cast<size_t>(p_.main_fn)], "dp_entry");
    out += "int main(int argc, char** argv) {\n" + uses() + "    dp_set_args(argc, argv);\n    return dp_run(dp_entry);\n}\n";
    return out;
}

// Вход сборки тестов (Р50): `программа N` исполняет тест номер N, без
// аргумента — печатает их число.
std::string Emitter::test_entry() {
    std::string out;
    for (size_t i = 0; i < tests_->size(); i++) out += wrapper(p_.fns[(*tests_)[i]], "dp_test" + std::to_string(i));
    out += "static const dp_entry_fn dp_tests[] = {";
    for (size_t i = 0; i < tests_->size(); i++) out += "dp_test" + std::to_string(i) + ", ";
    out += "NULL};\n\n";
    out += "int main(int argc, char** argv) {\n" + uses();
    out += "    long n = " + std::to_string(tests_->size()) + ";\n";
    out += "    if (argc < 2) {\n        printf(\"%ld\\n\", n);\n        return 0;\n    }\n";
    out += "    char* end = NULL;\n    long i = strtol(argv[1], &end, 10);\n";
    out += "    if (*argv[1] == 0 || *end != 0 || i < 0 || i >= n) return 2;\n";
    out += "    dp_set_args(1, argv);  /* у теста нет аргументов программы */\n";
    out += "    return dp_run_test(dp_tests[i]);\n}\n";
    return out;
}

}  // namespace

CResult emit_c(const ir::Program& p, const SourceMap& sources) { return Emitter(p, sources, nullptr).run(); }

CResult emit_c_tests(const ir::Program& p, const SourceMap& sources, const std::vector<uint32_t>& tests) {
    return Emitter(p, sources, &tests).run();
}

}  // namespace dp::backend_c
