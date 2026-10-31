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

#include "support/c_names.h"

#include <cmath>
#include <cstdio>
#include <functional>
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

// ?H, ?extern fn и ?&T (Р62) — указатель C, `none` — NULL: раскладка та же,
// что у C, в полях `@c`, за `&mut` и в списках.
bool niche_elem(const Type* e) { return e->kind == TK::Handle || e->kind == TK::CFn || e->kind == TK::Ref; }
bool niche(const Type* t) { return t->kind == TK::Optional && niche_elem(t->elem); }

bool is_scalar(const Type* t) {
    if (niche(t)) return true;
    switch (t->kind) {
        case TK::Bool: case TK::I8: case TK::I16: case TK::I32: case TK::I64: case TK::Int: case TK::U8:
        case TK::U16: case TK::U32: case TK::U64: case TK::F32: case TK::F64: case TK::Handle: case TK::CFn:
        case TK::Ref: case TK::Atomic:
            return true;
        default: return false;
    }
}

bool is_composite(const Type* t) {
    if (niche(t)) return false;
    switch (t->kind) {
        case TK::List: case TK::Map: case TK::Slice: case TK::Optional: case TK::Result: case TK::Struct: case TK::Enum:
        case TK::Array: case TK::Pool: case TK::Mutex: case TK::Channel:
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
        case TK::Pool: return "Q_" + mangle(t->elem);  // Р67
        case TK::Mutex: return "X_" + mangle(t->elem);  // Р70
        case TK::Channel: return "K_" + mangle(t->elem);
        case TK::PoolHandle: return "hnd";             // dp_hnd из runtime: один на все T
        case TK::Handle: return "H_" + lp(t->hdef->module->name) + lp(t->hdef->name);
        case TK::Slice: return "S_" + mangle(t->elem);
        case TK::Optional: return "O_" + mangle(t->elem);
        case TK::Result: return "R_" + (t->err ? mangle(t->err) : std::string("x")) + "_" + mangle(t->elem);
        case TK::Struct: return "T_" + lp(t->sdef->module->name) + lp(t->sdef->name);
        case TK::Enum: return "E_" + lp(t->edef->module->name) + lp(t->edef->name);
        case TK::Array: return "A" + std::to_string(t->len) + "_" + mangle(t->elem);  // Р62
        case TK::Ref: return "P_" + mangle(t->elem);
        case TK::CFn: {  // F_<число параметров><режим и тип>…<результат>; записи с длиной
            std::string s = "F_" + std::to_string(t->sig->params.size());
            for (const auto& [mode, p] : t->sig->params) {
                s += (mode == ast::ParamMode::Value ? "v" : mode == ast::ParamMode::Ref ? "r" : "m") + lp(mangle(p));
            }
            return s + lp(mangle(t->sig->ret));
        }
        default: return "error";
    }
}

std::string ctype(const Type* t) {
    if (niche(t)) return ctype(t->elem);
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
        case TK::Handle: return "void*";  // `extern type` — дескриптор C (Р59)
        case TK::Ref: return ctype(t->elem) + "*";  // Р62: адрес на время вызова C
        case TK::Atomic: return ctype(t->elem);    // Р70: операции — __atomic_*
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
    if (t->kind == TK::PoolHandle) return "dp_hnd_eq(*(" + a + "), *(" + b + "))";
    if (t->kind == TK::Str) return "dp_str_eq(*(" + a + "), *(" + b + "))";
    return "dp_eq__" + mangle(t) + "(" + a + ", " + b + ")";
}

std::string lt_of(const Type* t, const std::string& a, const std::string& b) {
    // ?T, E!T и указатель на функцию C интерпретатор не упорядочивает:
    // «меньше» всегда ложно. Сравнение `<` указателей на функции C11
    // запрещает (6.5.8), а сравнение списка его порождает.
    if (t->kind == TK::Optional || t->kind == TK::Result || t->kind == TK::CFn || t->kind == TK::PoolHandle) {
        return "false";
    }
    if (is_scalar(t)) return "(*(" + a + ") < *(" + b + "))";
    if (t->kind == TK::Str) return "dp_str_lt(*(" + a + "), *(" + b + "))";
    return "dp_lt__" + mangle(t) + "(" + a + ", " + b + ")";
}

// Запись «есть значение v» в место r типа ?T (elem — T); ind — отступ
// второй строки.
std::string set_some(const Type* elem, const std::string& r, const std::string& v, const std::string& ind) {
    if (niche_elem(elem)) return r + " = " + v + ";";
    return r + ".has = true;\n" + ind + r + ".value = " + v + ";";
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
    Emitter(const ir::Program& p, const SourceMap& sources, const std::vector<uint32_t>* tests, bool lib = false,
            std::string lib_name = "")
        : p_(p), src_(sources), tests_(tests), lib_(lib), lib_name_(std::move(lib_name)) {}

    CResult run() {
        for (const Function& f : p_.fns) collect(f);
        std::string out;
        out += "// Сгенерировано C-бэкендом dpl — не править.\n";
        out += "#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n#include \"dp_rt.h\"\n\n";
        out += strings();
        out += type_decls();
        out += helpers();
        for (const Function& f : p_.fns) {
            if (f.slot < 0) out += (f.is_extern ? extern_head(f) : fn_head(f)) + ";\n";  // у `@slot` символа нет
        }
        for (const Function& f : p_.fns) {
            if (f.exported) out += export_head(f) + " __asm__(" + cstr(f.c_name) + ");\n";
        }
        out += "\n";
        out += par_defs();
        out += task_defs();
        for (const Function& f : p_.fns) {
            if (!f.is_extern) out += function(f);
        }
        for (const Function& f : p_.fns) {
            if (f.exported) out += export_wrapper(f);
        }
        CResult r{out, errors_, p_.links, ""};
        if (lib_) r.header = header();  // библиотека для хоста C (Р61): входа нет
        else r.code += tests_ ? test_entry() : entry();
        r.errors = errors_;
        return r;
    }

    // Функция C (Р59): объявление под своим именем dpx_… с меткой символа
    // C — так оно не спорит с объявлениями заголовков библиотеки C.
    static std::string extern_name(const Function& f) {
        size_t dot = f.name.find('.');
        return "dpx_" + lp(f.name.substr(0, dot)) + lp(f.name.substr(dot + 1));
    }

    static ast::ParamMode mode_of(const ir::Local& l) {
        return l.kind == LocalKind::ParamRef ? ast::ParamMode::Ref
               : l.kind == LocalKind::ParamMut ? ast::ParamMode::Mut
                                               : ast::ParamMode::Value;
    }

    // Параметр на границе C (Р59, Р61); type — в записи dpl, ctype — в C программы.
    using CType = std::function<std::string(const Type*)>;

    static std::string c_param_type(ast::ParamMode mode, const Type* t, const CType& ct = ctype) {
        if (mode == ast::ParamMode::Ref) return "const " + ct(t) + "*";
        if (mode == ast::ParamMode::Mut) return (t->kind == TK::List ? ct(t->elem) : ct(t)) + "*";
        if (t->kind == TK::Str) return "const char*";
        if (t->kind == TK::Slice) return t->elem->kind == TK::Str ? "const char* const*" : "const " + ct(t->elem) + "*";
        if (t->kind == TK::Optional) {  // ?H, ?extern fn, ?&T (Р62), ?str — или NULL
            if (t->elem->kind == TK::Ref) return "const " + ct(t->elem->elem) + "*";
            if (t->elem->kind == TK::Str) return "const char*";
            return t->elem->kind == TK::CFn ? ct(t->elem) : "void*";
        }
        return ct(t);
    }

    static std::string c_result_type(const Type* t, const CType& ct = ctype) {
        if (t->kind == TK::Str || (t->kind == TK::Optional && t->elem->kind == TK::Str)) return "const char*";
        if (t->kind == TK::Optional) return t->elem->kind == TK::CFn ? ct(t->elem) : "void*";
        return ct(t);
    }

    std::string extern_head(const Function& f) {
        std::string h = "extern " + c_result_type(f.ret) + " " + extern_name(f) + "(";
        for (uint32_t i = 0; i < f.param_count; i++) h += (i ? ", " : "") + c_param_type(mode_of(f.locals[i]), f.locals[i].type);
        if (f.param_count == 0) h += "void";
        return h + ") __asm__(" + cstr(f.c_name) + ")";
    }

    // Функция `@export` (Р61): обёртка с ABI C под именем dpe_…; символ —
    // меткой __asm__, так имя C не спорит с заголовками libc.
    static std::string export_name(const Function& f) {
        size_t dot = f.name.find('.');
        return "dpe_" + lp(f.name.substr(0, dot)) + lp(f.name.substr(dot + 1));
    }

    std::string export_head(const Function& f) {
        std::string h = c_result_type(f.ret) + " " + export_name(f) + "(";
        for (uint32_t i = 0; i < f.param_count; i++) {
            h += (i ? ", " : "") + c_param_type(mode_of(f.locals[i]), f.locals[i].type) + " a" + std::to_string(i);
        }
        if (f.param_count == 0) h += "void";
        return h + ")";
    }

    std::string export_wrapper(const Function& f);
    std::string header();

    // Для FnEmitter.
    // Имя функции в IR — «модуль.функция»; в C — dpf_<модуль><функция> с длинами.
    static std::string fn_name(const Function& f) {
        size_t dot = f.name.find('.');
        if (dot == std::string::npos) return "dpf_" + lp(f.name);
        size_t hash = f.name.find('#');  // тело `parallel for` (Р69): «модуль.функция#номер»
        if (hash != std::string::npos) {
            return "dpp_" + lp(f.name.substr(0, dot)) + lp(f.name.substr(dot + 1, hash - dot - 1)) + "_" +
                   f.name.substr(hash + 1);
        }
        return "dpf_" + lp(f.name.substr(0, dot)) + lp(f.name.substr(dot + 1));
    }

    // `parallel for` (Р69): у каждого ParFor — окружение (нижняя граница и
    // указатели на захваченные места) и функция-трамплин для runtime.
    std::map<const ir::Rvalue*, uint32_t> par_ids_;

    uint32_t par_id(const ir::Rvalue& rv) const {
        auto it = par_ids_.find(&rv);
        return it == par_ids_.end() ? 0 : it->second;
    }

    std::string par_defs() {
        std::string out;
        for (const Function& f : p_.fns) {
            for (const ir::Block& b : f.blocks) {
                for (const ir::Inst& in : b.insts) {
                    if (in.rv.kind != RvKind::ParFor || in.rv.fn >= p_.fns.size()) continue;
                    uint32_t id = static_cast<uint32_t>(par_ids_.size());
                    par_ids_[&in.rv] = id;
                    const Function& g = p_.fns[in.rv.fn];
                    std::string n = std::to_string(id);
                    std::string t = ctype(g.locals[0].type);
                    out += "struct dp_pfe" + n + " {\n    " + t + " lo;\n";
                    for (uint32_t i = 1; i < g.param_count; i++) {
                        out += "    " + ctype(g.locals[i].type) + "* a" + std::to_string(i) + ";\n";
                    }
                    out += "};\n\nstatic void dp_pfb" + n + "(void* env, uint64_t k) {\n";
                    out += "    struct dp_pfe" + n + "* e = (struct dp_pfe" + n + "*)env;\n";
                    out += "    " + fn_name(g) + "((" + t + ")((uint64_t)e->lo + k)";
                    for (uint32_t i = 1; i < g.param_count; i++) out += ", e->a" + std::to_string(i);
                    out += ");\n}\n\n";
                }
            }
        }
        return out;
    }

    // Задача блока `tasks` (Р70): окружение — значения-аргументы и
    // указатели на места; runtime копирует его и зовёт трамплин в потоке
    // задачи.
    std::map<const ir::Rvalue*, uint32_t> task_ids_;

    uint32_t task_id(const ir::Rvalue& rv) const {
        auto it = task_ids_.find(&rv);
        return it == task_ids_.end() ? 0 : it->second;
    }

    std::string task_defs() {
        std::string out;
        for (const Function& f : p_.fns) {
            for (const ir::Block& b : f.blocks) {
                for (const ir::Inst& in : b.insts) {
                    if (in.rv.kind != RvKind::Spawn || in.rv.fn >= p_.fns.size()) continue;
                    uint32_t id = static_cast<uint32_t>(task_ids_.size());
                    task_ids_[&in.rv] = id;
                    const Function& g = p_.fns[in.rv.fn];
                    std::string n = std::to_string(id);
                    out += "struct dp_tke" + n + " {\n";
                    for (uint32_t i = 0; i < g.param_count; i++) {
                        const ir::Local& l = g.locals[i];
                        bool ref = l.kind == LocalKind::ParamRef || l.kind == LocalKind::ParamMut;
                        out += "    " + ctype(l.type) + (ref ? "* a" : " a") + std::to_string(i) + ";\n";
                    }
                    if (g.param_count == 0) out += "    char unused;\n";
                    out += "};\n\nstatic void dp_tkb" + n + "(void* env) {\n";
                    out += "    struct dp_tke" + n + "* e = (struct dp_tke" + n + "*)env;\n";
                    out += "    " + (g.param_count == 0 ? std::string("(void)e;\n    ") : std::string()) + fn_name(g) + "(";
                    for (uint32_t i = 0; i < g.param_count; i++) out += std::string(i ? ", " : "") + "e->a" + std::to_string(i);
                    out += ");\n}\n\n";
                }
            }
        }
        return out;
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
    bool lib_ = false;                              // `dpl build --lib` (Р61): без входа, с заголовком
    std::string lib_name_;                          // имя библиотеки: метка заголовка
    std::vector<std::string> errors_;
    std::vector<const Type*> types_;  // составные типы в порядке определения
    std::vector<const Type*> cfns_;   // типы `extern fn(...)`: typedef указателя до определений
    std::set<const Type*> seen_;
    std::set<std::string> mangled_;

    void error(const std::string& msg) { errors_.push_back(msg); }

    // --- сбор типов

    void add_type(const Type* t) {
        if (!t || seen_.count(t)) return;
        seen_.insert(t);
        switch (t->kind) {
            case TK::List: case TK::Slice: case TK::Array: case TK::Ref: case TK::Pool: case TK::Mutex: case TK::Channel:
                add_type(t->elem);
                break;
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
            case TK::CFn:
                for (const auto& p : t->sig->params) add_type(p.second);
                add_type(t->sig->ret);
                cfns_.push_back(t);
                break;
            default: break;
        }
        // Разные типы с одной записью C (List[Handle[A]] и List[Handle[B]] — dp_L_hnd, Р67) — одно определение.
        if (is_composite(t) && !mangled_.count(mangle(t))) types_.push_back(t);
        if (is_composite(t) || niche(t)) mangled_.insert(mangle(t));  // ?H — для pop и get
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
        for (const Type* t : types_) {
            const char* tag = t->kind == TK::Struct && t->sdef->union_ ? "union " : "struct ";
            out += "typedef " + std::string(tag) + ctype(t) + " " + ctype(t) + ";\n";
        }
        // Указатель на функцию C (Р61): типы параметров здесь могут быть неполными.
        for (const Type* t : cfns_) out += cfn_typedef(t, ctype(t), ctype);
        out += "\n";
        for (const Type* t : types_) out += type_def(t);
        return out;
    }

    // Поле заимствующей структуры в раскладке C (Р62): строка — const char*,
    // срез и ссылка — указатель на элементы, ?-указатель — указатель или NULL.
    static std::string c_field_ctype(const Type* t) {
        switch (t->kind) {
            case TK::Str: return "const char*";
            case TK::Slice: return t->elem->kind == TK::Str ? "const char**" : ctype(t->elem) + "*";
            case TK::Ref: return ctype(t->elem) + "*";
            case TK::Optional:
                if (t->elem->kind == TK::Ref) return ctype(t->elem->elem) + "*";
                return t->elem->kind == TK::CFn ? ctype(t->elem) : "void*";
            default: return ctype(t);
        }
    }

    static std::string cfn_typedef(const Type* t, const std::string& name, const CType& ct) {
        std::string s = "typedef " + c_result_type(t->sig->ret, ct) + " (*" + name + ")(";
        for (size_t i = 0; i < t->sig->params.size(); i++) {
            s += (i ? ", " : "") + c_param_type(t->sig->params[i].first, t->sig->params[i].second, ct);
        }
        return s + (t->sig->params.empty() ? "void" : "") + ");\n";
    }

    std::string type_def(const Type* t) {
        std::string n = ctype(t);
        std::string out = (t->kind == TK::Struct && t->sdef->union_ ? "union " : "struct ") + n + " {\n";
        switch (t->kind) {
            case TK::List: out += "    " + ctype(t->elem) + "* ptr;\n    int64_t len;\n    int64_t cap;\n"; break;
            case TK::Map:  // Р54: пары в порядке вставки и хэш-индекс позиций (позиция + 1, 0 — пусто)
                out += "    " + ctype(t->key) + "* keys;\n    " + ctype(t->elem) +
                       "* vals;\n    int64_t len;\n    int64_t cap;\n    int64_t* slots;\n    int64_t nslots;\n";
                break;
            case TK::Slice: out += "    " + ctype(t->elem) + "* ptr;\n    int64_t len;\n"; break;
            // Р67: значения слотов и служебная часть той же ёмкости (dp_pool, runtime).
            case TK::Pool: out += "    " + ctype(t->elem) + "* vals;\n    dp_pool m;\n"; break;
            // Р70: мьютекс runtime и данные.
            case TK::Mutex: out += "    dp_mtx mx;\n    " + ctype(t->elem) + " v;\n"; break;
            // Синхронизация — в runtime, значения — кольцо ёмкости канала.
            case TK::Channel: out += "    dp_chan* c;\n    " + ctype(t->elem) + "* buf;\n"; break;
            // Массив C (Р62) — в обёртке: так его можно присвоить; раскладка — как у массива.
            case TK::Array: out += "    " + ctype(t->elem) + " v[" + std::to_string(t->len) + "];\n"; break;
            case TK::Optional: out += "    bool has;\n    " + ctype(t->elem) + " value;\n"; break;
            case TK::Result:
                out += "    bool is_err;\n";
                if (t->elem->kind != TK::Void) out += "    " + ctype(t->elem) + " value;\n";
                if (t->err) out += "    " + ctype(t->err) + " err;\n";
                break;
            case TK::Struct:
                if (t->sdef->fields.empty()) out += "    char dp_empty;\n";
                // Р62: у заимствующей структуры поля — сразу в раскладке C.
                for (const sema::FieldDef& f : t->sdef->fields) {
                    out += "    " + (sema::c_borrowing(t) ? c_field_ctype(f.type) : ctype(f.type)) + " f_" + f.name + ";\n";
                }
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
            if (t->kind == TK::Pool) def += pool_methods(t);
            if (t->kind == TK::Channel) def += channel_methods(t);
        }
        return decl + "\n" + def;
    }

    std::string eq_def(const Type* t) {
        std::string n = ctype(t), m = mangle(t);
        std::string b = "DP_INLINE bool dp_eq__" + m + "(const " + n + "* a, const " + n + "* b) {\n";
        if (sema::c_borrowing(t)) return b + "    (void)a;\n    (void)b;\n    return false;\n}\n\n";  // Р62: не сравнивается
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
            case TK::Pool: case TK::Mutex: case TK::Channel:  // и пулы (Р67)
                b += "    (void)a;\n    (void)b;\n    return false;\n";
                break;
            // Р62: массив и объединение dpl не сравнивает — функция нужна для сборки C.
            case TK::Array: b += "    (void)a;\n    (void)b;\n    return false;\n"; break;
            case TK::Struct:
                if (t->sdef->union_) {
                    b += "    (void)a;\n    (void)b;\n    return false;\n";
                    break;
                }
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
        if (sema::c_borrowing(t)) return b + "    (void)a;\n    (void)b;\n    return false;\n}\n\n";
        switch (t->kind) {
            case TK::List: case TK::Slice:
                b += "    int64_t n = a->len < b->len ? a->len : b->len;\n    for (int64_t i = 0; i < n; i++) {\n";
                b += "        if (" + lt_of(t->elem, "&a->ptr[i]", "&b->ptr[i]") + ") return true;\n";
                b += "        if (" + lt_of(t->elem, "&b->ptr[i]", "&a->ptr[i]") + ") return false;\n";
                b += "    }\n    return a->len < b->len;\n";
                break;
            case TK::Optional: case TK::Result: case TK::Map: case TK::Array: case TK::Pool: case TK::Mutex:
            case TK::Channel:
                b += "    (void)a;\n    (void)b;\n    return false;\n";
                break;
            case TK::Struct: {
                if (t->sdef->union_) {  // Р62: объединение не упорядочивается
                    b += "    (void)a;\n    (void)b;\n    return false;\n";
                    break;
                }
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
            case TK::Pool:
                if (ir::needs_drop(t->elem)) {
                    b += "    for (int64_t i = 0; i < v->m.nslots; i++) {\n        if (v->m.live[i]) " +
                         drop_of(t->elem, "v->vals[i]") + "\n    }\n";
                }
                b += "    dp_free(v->vals);\n    dp_pool_drop(&v->m);\n";
                break;
            case TK::Mutex: b += "    dp_mtx_drop(&v->mx);\n" + drop_line(t->elem, "v->v", "    "); break;
            case TK::Channel:
                b += "    if (v->c) {\n";
                if (ir::needs_drop(t->elem)) {
                    b += "        int64_t cap = dp_chan_cap(v->c), head = dp_chan_head(v->c), n = dp_chan_len(v->c);\n";
                    b += "        for (int64_t i = 0; i < n; i++) " + drop_of(t->elem, "v->buf[(head + i) % cap]") + "\n";
                }
                b += "        dp_chan_free(v->c);\n    }\n    dp_free(v->buf);\n";
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
            case TK::Mutex: case TK::Channel:  // Р70: не копируются (E0518) — функция нужна для сборки C
                b += "    " + n + " r = {0};\n    (void)v;\n    dp_unreachable(\"copy of a Mutex\");\n";
                break;
            case TK::Pool: {  // Р67: копируются живые значения; номера и поколения те же
                std::string ec = ctype(t->elem);
                b += "    " + n + " r = {0};\n    r.m = dp_pool_copy(&v->m);\n";
                b += "    r.vals = (" + ec + "*)dp_alloc(r.m.cap, sizeof(" + ec + "), NULL);\n";
                b += "    for (int64_t i = 0; i < v->m.nslots; i++) {\n        if (v->m.live[i]) r.vals[i] = " +
                     copy_of(t->elem, "v->vals[i]") + ";\n    }\n";
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
    // (заполнен не больше чем наполовину). Пары лежат в порядке вставки, на
    // место удалённой встаёт последняя; наружу порядок не виден: ключи
    // отдаются по возрастанию (dp_mkeys).
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
        // Индекс заново под want пар: размер — степень двойки не меньше 2 * want + 2.
        b += "DP_INLINE void dp_mindex__" + m + "(" + n + "* m, int64_t want, const char* loc) {\n";
        b += "    int64_t ns = 8;\n    while (ns < 2 * want + 2) ns *= 2;\n    dp_free(m->slots);\n";
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
        b += "    if (2 * m->len + 2 > m->nslots) {\n        dp_mindex__" + m + "(m, m->len, loc);\n        return;\n    }\n";
        b += "    uint64_t mask = (uint64_t)m->nslots - 1;\n";
        b += "    uint64_t i = " + hash_of(t->key, "m->keys[m->len - 1]") + " & mask;\n";
        b += "    while (m->slots[i] != 0) i = (i + 1) & mask;\n    m->slots[i] = m->len;\n}\n\n";
        b += "DP_INLINE " + vc + "* dp_mvalue__" + m + "(" + n + "* m, const " + kc + "* key, const char* loc) {\n";
        b += "    int64_t p = dp_mfind__" + m + "(m, key);\n";
        b += "    if (p < 0) dp_trap(loc, \"key is not in the map\");\n    return &m->vals[p];\n}\n\n";
        // Ёмкость не меньше n пар (Р68): массивы пар и индекс — сразу.
        b += "DP_INLINE void dp_mreserve__" + m + "(" + n + "* m, int64_t n, const char* loc) {\n";
        b += "    dp_check_reserve(n, loc);\n    if (n > m->cap) {\n";
        b += "        m->keys = (" + kc + "*)dp_realloc(m->keys, n, sizeof(" + kc + "), loc);\n";
        b += "        m->vals = (" + vc + "*)dp_realloc(m->vals, n, sizeof(" + vc + "), loc);\n        m->cap = n;\n    }\n";
        b += "    if (2 * n + 2 > m->nslots) dp_mindex__" + m + "(m, n, loc);\n}\n\n";
        b += "DP_INLINE bool dp_mcontains__" + m + "(const " + n + "* m, const " + kc + "* key) {\n";
        b += "    return dp_mfind__" + m + "(m, key) >= 0;\n}\n\n";
        b += "DP_INLINE void dp_mclear__" + m + "(" + n + "* m) {\n";
        if (ir::needs_drop(t->key) || ir::needs_drop(t->elem)) {
            b += "    for (int64_t i = 0; i < m->len; i++) {\n" + drop_stmt(t->key, "m->keys[i]") +
                 drop_stmt(t->elem, "m->vals[i]") + "    }\n";
        }
        b += "    m->len = 0;\n    if (m->nslots > 0) memset(m->slots, 0, (size_t)m->nslots * sizeof(int64_t));\n}\n\n";
        if (mangled_.count("O_" + vm)) {
            std::string o = niche_elem(t->elem) ? ctype(t->elem) : "dp_O_" + vm;
            b += "DP_INLINE " + o + " dp_mget__" + m + "(const " + n + "* m, const " + kc + "* key) {\n";
            b += "    " + o + " r = {0};\n    int64_t p = dp_mfind__" + m + "(m, key);\n";
            b += "    if (p >= 0) {\n        " + set_some(t->elem, "r", copy_of(t->elem, "m->vals[p]"), "        ") +
                 "\n    }\n    return r;\n}\n\n";
            // Удаление — O(1) в среднем: слот пары убирается сдвигом назад
            // (линейное пробирование), на место пары встаёт последняя.
            b += "DP_INLINE " + o + " dp_mremove__" + m + "(" + n + "* m, " + kc + " key, const char* loc) {\n";
            b += "    (void)loc;\n    " + o + " r = {0};\n    int64_t p = dp_mfind__" + m + "(m, &key);\n" +
                 drop_stmt(t->key, "key");
            b += "    if (p < 0) return r;\n    " + set_some(t->elem, "r", "m->vals[p]", "    ") + "\n";
            b += "    uint64_t mask = (uint64_t)m->nslots - 1;\n";
            b += "    uint64_t i = " + hash_of(t->key, "m->keys[p]") + " & mask;\n";
            b += "    while (m->slots[i] != p + 1) i = (i + 1) & mask;\n" + drop_stmt(t->key, "m->keys[p]");
            b += "    for (uint64_t j = (i + 1) & mask; m->slots[j] != 0; j = (j + 1) & mask) {\n";
            b += "        uint64_t h = " + hash_of(t->key, "m->keys[m->slots[j] - 1]") + " & mask;\n";
            b += "        if (((j - h) & mask) >= ((j - i) & mask)) {\n";
            b += "            m->slots[i] = m->slots[j];\n            i = j;\n        }\n    }\n";
            b += "    m->slots[i] = 0;\n    int64_t last = m->len - 1;\n    if (p != last) {\n";
            b += "        uint64_t k = " + hash_of(t->key, "m->keys[last]") + " & mask;\n";
            b += "        while (m->slots[k] != last + 1) k = (k + 1) & mask;\n";
            b += "        m->slots[k] = p + 1;\n        m->keys[p] = m->keys[last];\n        m->vals[p] = m->vals[last];\n";
            b += "    }\n    m->len--;\n    return r;\n}\n\n";
        }
        if (mangled_.count("L_" + km)) {
            std::string l = "dp_L_" + km;
            b += "DP_INLINE " + l + " dp_mkeys__" + m + "(const " + n + "* m) {\n    " + l + " r = {0};\n";
            b += "    r.ptr = (" + kc + "*)dp_alloc(m->len, sizeof(" + kc + "), NULL);\n    r.len = m->len;\n    r.cap = m->len;\n";
            b += "    for (int64_t i = 0; i < m->len; i++) r.ptr[i] = " + copy_of(t->key, "m->keys[i]") + ";\n";
            b += "    dp_sort__" + std::string("L_") + km + "(&r);\n    return r;\n}\n\n";
        }
        if (mangled_.count("L_i64")) {
            // Номера пар по возрастанию ключа — для обхода (Р54). Сортируются
            // пары «ключ, номер»; ключ — побитовая копия без владения, строки
            // не копируются. Слияние устойчиво — порядок тот же, что у keys().
            // Ключи различны, поэтому любая верная сортировка даёт тот же
            // порядок: целые и bool — поразрядно (LSD по байтам, знак — сдвигом
            // к беззнаковому), строки — слиянием.
            std::string kp = "dp_kp__" + m;
            bool radix = sema::is_integer(t->key) || t->key->kind == TK::Bool;
            b += "DP_INLINE dp_L_i64 dp_morder__" + m + "(const " + n + "* m) {\n";
            b += "    dp_L_i64 r = {0};\n    int64_t n = m->len;\n    if (n == 0) return r;\n";
            b += "    struct " + kp + " { " + (radix ? std::string("uint64_t") : kc) + " k; int64_t p; };\n";
            b += "    struct " + kp + "* a = (struct " + kp + "*)dp_alloc(n, sizeof(struct " + kp + "), NULL);\n";
            b += "    struct " + kp + "* b = (struct " + kp + "*)dp_alloc(n, sizeof(struct " + kp + "), NULL);\n";
            if (radix) {
                std::string key = sema::is_signed(t->key)
                                      ? "(uint64_t)(int64_t)m->keys[i] ^ UINT64_C(0x8000000000000000)"
                                      : "(uint64_t)m->keys[i]";
                b += "    for (int64_t i = 0; i < n; i++) {\n        a[i].k = " + key + ";\n        a[i].p = i;\n    }\n";
                b += "    for (int s = 0; s < 64; s += 8) {\n        int64_t c[257] = {0};\n";
                b += "        for (int64_t i = 0; i < n; i++) c[((a[i].k >> s) & 255) + 1]++;\n";
                b += "        int same = 0;\n        for (int d = 1; d <= 256; d++) same |= c[d] == n;\n";
                b += "        if (same) continue;\n        for (int d = 1; d <= 256; d++) c[d] += c[d - 1];\n";
                b += "        for (int64_t i = 0; i < n; i++) b[c[(a[i].k >> s) & 255]++] = a[i];\n";
                b += "        struct " + kp + "* t = a;\n        a = b;\n        b = t;\n    }\n";
            } else {
                b += "    for (int64_t i = 0; i < n; i++) {\n        a[i].k = m->keys[i];\n        a[i].p = i;\n    }\n";
                b += "    for (int64_t w = 1; w < n; w *= 2) {\n        for (int64_t lo = 0; lo < n; lo += 2 * w) {\n";
                b += "            int64_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;\n";
                b += "            int64_t i = lo, j = mid, k = lo;\n";
                b += "            while (i < mid && j < hi) {\n                if (" + lt_of(t->key, "&a[j].k", "&a[i].k") +
                     ") b[k++] = a[j++];\n                else b[k++] = a[i++];\n            }\n";
                b += "            while (i < mid) b[k++] = a[i++];\n            while (j < hi) b[k++] = a[j++];\n        }\n";
                b += "        struct " + kp + "* t = a;\n        a = b;\n        b = t;\n    }\n";
            }
            b += "    r.ptr = (int64_t*)dp_alloc(n, sizeof(int64_t), NULL);\n    r.len = n;\n    r.cap = n;\n";
            b += "    for (int64_t i = 0; i < n; i++) r.ptr[i] = a[i].p;\n";
            b += "    dp_free(a);\n    dp_free(b);\n    return r;\n}\n\n";
        }
        return b;
    }

    // Методы Pool (Р67). Служебная часть — dp_pool из runtime; значения —
    // в vals той же ёмкости: она растёт в dp_pool_take и dp_pool_grow.
    // Channel[T] (Р70): значения — в кольце buf, ожидание и номера
    // ячеек — dp_chan_* runtime.
    std::string channel_methods(const Type* t) {
        std::string n = ctype(t), m = mangle(t), ec = ctype(t->elem), em = mangle(t->elem);
        std::string b;
        b += "DP_INLINE " + n + " dp_cnew__" + m + "(int64_t cap, const char* loc) {\n    " + n + " r;\n";
        b += "    r.c = dp_chan_new(cap, loc);\n    r.buf = (" + ec + "*)dp_alloc(cap, sizeof(" + ec + "), loc);\n";
        b += "    return r;\n}\n\n";
        b += "DP_INLINE void dp_csend__" + m + "(" + n + "* k, " + ec + " v, const char* loc) {\n";
        b += "    int64_t s = dp_chan_send_begin(k->c, loc);\n    k->buf[s] = v;\n    dp_chan_send_end(k->c);\n}\n\n";
        if (mangled_.count("O_" + em) || niche_elem(t->elem)) {
            std::string o = niche_elem(t->elem) ? ec : "dp_O_" + em;
            b += "DP_INLINE " + o + " dp_crecv__" + m + "(" + n + "* k, bool wait, const char* loc) {\n    " + o +
                 " r = {0};\n    int64_t s = 0;\n";
            b += "    if (dp_chan_recv_begin(k->c, wait, &s, loc)) {\n        " + set_some(t->elem, "r", "k->buf[s]", "        ") +
                 "\n        dp_chan_recv_end(k->c);\n    }\n    return r;\n}\n\n";
        }
        return b;
    }

    std::string pool_methods(const Type* t) {
        std::string n = ctype(t), m = mangle(t), ec = ctype(t->elem), em = mangle(t->elem);
        std::string b;
        auto grow_vals = [&](const std::string& ind) {
            return ind + "if (p->m.cap != cap) p->vals = (" + ec + "*)dp_realloc(p->vals, p->m.cap, sizeof(" + ec +
                   "), loc);\n";
        };
        b += "DP_INLINE dp_hnd dp_padd__" + m + "(" + n + "* p, " + ec + " v, const char* loc) {\n";
        b += "    int64_t cap = p->m.cap;\n    int64_t i = dp_pool_take(&p->m, loc);\n" + grow_vals("    ");
        b += "    p->vals[i] = v;\n    dp_hnd h = {(uint32_t)i, p->m.gens[i]};\n    return h;\n}\n\n";
        b += "DP_INLINE " + ec + "* dp_pvalue__" + m + "(" + n + "* p, dp_hnd h, const char* loc) {\n";
        b += "    if (!dp_pool_valid(&p->m, h)) dp_trap(loc, \"stale handle: its slot was removed or reused\");\n";
        b += "    return &p->vals[h.index];\n}\n\n";
        b += "DP_INLINE bool dp_pcontains__" + m + "(const " + n + "* p, dp_hnd h) { return dp_pool_valid(&p->m, h); }\n\n";
        if (mangled_.count("O_" + em)) {
            std::string o = niche_elem(t->elem) ? ec : "dp_O_" + em;
            b += "DP_INLINE " + o + " dp_pget__" + m + "(const " + n + "* p, dp_hnd h) {\n    " + o + " r = {0};\n";
            b += "    if (dp_pool_valid(&p->m, h)) {\n        " +
                 set_some(t->elem, "r", copy_of(t->elem, "p->vals[h.index]"), "        ") + "\n    }\n    return r;\n}\n\n";
            b += "DP_INLINE " + o + " dp_premove__" + m + "(" + n + "* p, dp_hnd h) {\n    " + o + " r = {0};\n";
            b += "    if (dp_pool_valid(&p->m, h)) {\n        " + set_some(t->elem, "r", "p->vals[h.index]", "        ") +
                 "\n        dp_pool_release(&p->m, h.index);\n    }\n    return r;\n}\n\n";
        }
        b += "DP_INLINE void dp_pclear__" + m + "(" + n + "* p) {\n";
        if (ir::needs_drop(t->elem)) {
            b += "    for (int64_t i = 0; i < p->m.nslots; i++) {\n        if (p->m.live[i]) " +
                 drop_of(t->elem, "p->vals[i]") + "\n    }\n";
        }
        b += "    dp_pool_clear(&p->m);\n}\n\n";
        b += "DP_INLINE void dp_preserve__" + m + "(" + n + "* p, int64_t n, const char* loc) {\n";
        b += "    dp_check_reserve(n, loc);\n    int64_t cap = p->m.cap;\n    dp_pool_grow(&p->m, n, loc);\n" +
             grow_vals("    ") + "}\n\n";
        if (mangled_.count("L_hnd")) {
            b += "DP_INLINE dp_L_hnd dp_phandles__" + m + "(const " + n + "* p) {\n    dp_L_hnd r = {0};\n";
            b += "    r.ptr = (dp_hnd*)dp_alloc(p->m.len, sizeof(dp_hnd), NULL);\n    r.cap = p->m.len;\n";
            b += "    for (int64_t i = 0; i < p->m.nslots; i++) {\n        if (!p->m.live[i]) continue;\n";
            b += "        dp_hnd h = {(uint32_t)i, p->m.gens[i]};\n        r.ptr[r.len++] = h;\n    }\n    return r;\n}\n\n";
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
            std::string o = niche_elem(t->elem) ? ec : "dp_O_" + e;
            b += "DP_INLINE " + o + " dp_pop__" + m + "(" + n + "* l) {\n    " + o + " r = {0};\n";
            b += "    if (l->len > 0) {\n        " + set_some(t->elem, "r", "l->ptr[--l->len]", "        ") +
                 "\n    }\n    return r;\n}\n\n";
        }
        b += "DP_INLINE void dp_insert__" + m + "(" + n + "* l, int64_t i, " + ec + " v, const char* loc) {\n";
        b += "    dp_check_insert(i, l->len, loc);\n    dp_push__" + m + "(l, v, loc);\n";
        b += "    memmove(&l->ptr[i + 1], &l->ptr[i], (size_t)(l->len - 1 - i) * sizeof(" + ec + "));\n";
        b += "    l->ptr[i] = v;\n}\n\n";
        b += "DP_INLINE " + ec + " dp_remove__" + m + "(" + n + "* l, int64_t i, const char* loc) {\n";
        b += "    dp_check_index(i, l->len, loc);\n    " + ec + " v = l->ptr[i];\n";
        b += "    memmove(&l->ptr[i], &l->ptr[i + 1], (size_t)(l->len - 1 - i) * sizeof(" + ec + "));\n";
        b += "    l->len--;\n    return v;\n}\n\n";
        // Ёмкость не меньше n (Р68); clear() её сохраняет.
        b += "DP_INLINE void dp_reserve__" + m + "(" + n + "* l, int64_t n, const char* loc) {\n";
        b += "    dp_check_reserve(n, loc);\n    if (n > l->cap) {\n";
        b += "        l->ptr = (" + ec + "*)dp_realloc(l->ptr, n, sizeof(" + ec + "), loc);\n        l->cap = n;\n    }\n}\n\n";
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
                    if (t->kind == TK::PoolHandle) {  // Р67: index, generation
                        r.expr = "(" + r.expr + ")." + (pr.index == 0 ? "index" : "generation");
                        r.type = p_.types->prim(TK::U32);
                        break;
                    }
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
                    if (t->kind == TK::Array) {  // Р62: длина — в типе
                        line(ctype(t->elem) + "* " + ptr + " = &(" + r.expr + ").v[dp_check_index(" + idx + ", " +
                             std::to_string(t->len) + ", " + loc_ + ")];");
                        r.expr = "(*" + ptr + ")";
                        r.type = t->elem;
                        break;
                    }
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
                case ProjKind::Unwrap:
                    if (!niche(t)) r.expr = "(" + r.expr + ").value";  // ?H — сам указатель
                    r.type = t->elem;
                    break;
                case ProjKind::ResultValue: r.expr = "(" + r.expr + ").value"; r.type = t->elem; break;
                case ProjKind::ResultError: r.expr = "(" + r.expr + ").err"; r.type = t->err; break;
                case ProjKind::MapValue: {  // Р54
                    std::string ptr = fresh("dp_p");
                    line(ctype(t->elem) + "* " + ptr + " = dp_mvalue__" + mangle(t) + "(&(" + r.expr + "), &(" +
                         local_expr(pr.index) + "), " + loc_ + ");");
                    r.expr = "(*" + ptr + ")";
                    r.type = t->elem;
                    break;
                }
                case ProjKind::PoolValue: {  // Р67: устаревший хэндл — ловушка
                    std::string ptr = fresh("dp_p");
                    line(ctype(t->elem) + "* " + ptr + " = dp_pvalue__" + mangle(t) + "(&(" + r.expr + "), " +
                         local_expr(pr.index) + ", " + loc_ + ");");
                    r.expr = "(*" + ptr + ")";
                    r.type = t->elem;
                    break;
                }
                case ProjKind::MutexValue:  // данные Mutex (Р70)
                    r.expr = "(" + r.expr + ").v";
                    r.type = t->elem;
                    break;
                case ProjKind::PoolSlot:  // живой слот из обхода
                    r.expr = "(" + r.expr + ").vals[" + local_expr(pr.index) + "]";
                    r.type = t->elem;
                    break;
                case ProjKind::MapKeyAt: case ProjKind::MapValueAt: {  // номер пары из dp_morder
                    bool key = pr.kind == ProjKind::MapKeyAt;
                    r.expr = "(" + r.expr + ")." + (key ? "keys" : "vals") + "[" + local_expr(pr.index) + "]";
                    r.type = key ? t->key : t->elem;
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
            case Const::Kind::Fn: return "(" + ctype(t) + ")" + Emitter::export_name(p_.fns[c.fn]);
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
    std::string extern_call(const Rvalue& rv, const Function& g);
    std::string borrowing_struct(const Rvalue& rv);
    struct CParam {
        ast::ParamMode mode;
        const Type* type;
    };
    std::string c_call(const Rvalue& rv, const std::vector<CParam>& params, const Type* ret, const std::string& callee,
                       const std::string& what, const std::string& c_name);
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
        case ir::UnKind::Canon: return std::string(t->kind == TK::F32 ? "dp_canon_f32(" : "dp_canon_f64(") + a + ")";
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
        case CastKind::Widen: case CastKind::RefUp: return "((" + ct + ")" + a + ")";
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
        case CastKind::Bits: return std::string(from->kind == TK::F32 ? "dp_f32_bits(" : "dp_f64_bits(") + a + ")";
        case CastKind::FromBits:
            return std::string(to->kind == TK::F32 ? "dp_f32_from_bits(" : "dp_f64_from_bits(") + a + ")";
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
    if (rv.intrinsic == Intrinsic::Probe) {  // проверки места — строками place(), без чтения
        Lv p = place(rv.place);
        return "((void)" + p.expr + ")";
    }
    if (rv.has_place && rv.intrinsic == Intrinsic::ReadFile) {
        // __read_file (Р51): путь читается, текст — в место; результат — код.
        std::string path = operand(rv.ops[0], false);
        Lv out = place(rv.place);
        return "dp_read_file(" + path + ", &" + out.expr + ", " + loc_ + ")";
    }
    if (rv.has_place && rv.intrinsic == Intrinsic::FileRead) {
        // __file_read (Р71): байты дописываются в List[u8] места.
        std::string f = operand(rv.ops[0], false), n = operand(rv.ops[1], false);
        Lv b = place(rv.place);
        return "dp_file_read(" + f + ", &" + b.expr + ".ptr, &" + b.expr + ".len, &" + b.expr + ".cap, " + n + ", " +
               loc_ + ")";
    }
    if (rv.has_place && rv.intrinsic >= Intrinsic::AtomicLoad && rv.intrinsic <= Intrinsic::AtomicCas) {
        // Atomic[T] (Р70): T в памяти, операции — __atomic_* с
        // последовательной согласованностью; add и sub — ловушка переполнения.
        std::vector<std::string> ops;
        for (const Operand& o : rv.ops) ops.push_back(operand(o, false));
        Lv a = place(rv.place);
        std::string p = "&" + a.expr;
        const Type* t = a.type->elem;
        const std::string sc = "__ATOMIC_SEQ_CST";
        switch (rv.intrinsic) {
            case Intrinsic::AtomicLoad: return "__atomic_load_n(" + p + ", " + sc + ")";
            case Intrinsic::AtomicStore: return "__atomic_store_n(" + p + ", " + ops[0] + ", " + sc + ")";
            case Intrinsic::AtomicSwap: return "__atomic_exchange_n(" + p + ", " + ops[0] + ", " + sc + ")";
            case Intrinsic::AtomicCas: {
                std::string e = fresh("dp_ex");
                line(ctype(t) + " " + e + " = " + ops[0] + ";");
                return "__atomic_compare_exchange_n(" + p + ", &" + e + ", " + ops[1] + ", 0, " + sc + ", " + sc + ")";
            }
            default: {
                bool add = rv.intrinsic == Intrinsic::AtomicAdd;
                std::string old = fresh("dp_old");
                line(ctype(t) + " " + old + " = __atomic_fetch_" + (add ? "add" : "sub") + "(" + p + ", " + ops[0] +
                     ", " + sc + ");");
                bool sg = sema::is_signed(t);
                std::string cast = sg ? "(int64_t)" : "(uint64_t)";
                line("(void)dp_" + std::string(sg ? "s" : "u") + (add ? "add" : "sub") + "(" + cast + old + ", " + cast +
                     ops[0] + ", " + bits(t) + ", " + tname(t) + ", " + loc_ + ");");
                return old;
            }
        }
    }
    if (rv.has_place && rv.intrinsic >= Intrinsic::ChannelSend && rv.intrinsic <= Intrinsic::ChannelClose) {
        std::vector<std::string> ops;  // Р70: send отдаёт значение каналу
        for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
        Lv k = place(rv.place);
        std::string m = mangle(k.type), p = "&" + k.expr;
        switch (rv.intrinsic) {
            case Intrinsic::ChannelSend: return "dp_csend__" + m + "(" + p + ", " + ops[0] + ", " + loc_ + ")";
            case Intrinsic::ChannelClose: return "dp_chan_close((" + k.expr + ").c, " + loc_ + ")";
            default:
                return "dp_crecv__" + m + "(" + p + ", " + (rv.intrinsic == Intrinsic::ChannelRecv ? "true" : "false") +
                       ", " + loc_ + ")";
        }
    }
    if (rv.has_place && (rv.intrinsic == Intrinsic::MutexLock || rv.intrinsic == Intrinsic::MutexUnlock)) {
        Lv m = place(rv.place);  // Р70
        if (rv.intrinsic == Intrinsic::MutexUnlock) return "dp_mtx_unlock(&(" + m.expr + ").mx)";
        return "dp_mtx_lock(&(" + m.expr + ").mx, " + loc_ + ")";
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
            case Intrinsic::ListReserve:
                return "dp_reserve__" + m + "(" + l + ", (int64_t)" + ops[0] + ", " + loc_ + ")";
            case Intrinsic::MapReserve:
                return "dp_mreserve__" + m + "(" + l + ", (int64_t)" + ops[0] + ", " + loc_ + ")";
            case Intrinsic::MapSet: return "dp_mset__" + m + "(" + l + ", " + ops[0] + ", " + ops[1] + ", " + loc_ + ")";
            case Intrinsic::MapRemove: return "dp_mremove__" + m + "(" + l + ", " + ops[0] + ", " + loc_ + ")";
            case Intrinsic::MapClear: return "dp_mclear__" + m + "(" + l + ")";
            case Intrinsic::PoolAdd: return "dp_padd__" + m + "(" + l + ", " + ops[0] + ", " + loc_ + ")";
            case Intrinsic::PoolRemove: return "dp_premove__" + m + "(" + l + ", " + ops[0] + ")";
            case Intrinsic::PoolClear: return "dp_pclear__" + m + "(" + l + ")";
            case Intrinsic::PoolReserve:
                return "dp_preserve__" + m + "(" + l + ", (int64_t)" + ops[0] + ", " + loc_ + ")";
            default: error("unknown list method");
                return "0";
        }
    }
    std::vector<std::string> ops;
    for (const Operand& o : rv.ops) ops.push_back(operand(o, rv.intrinsic == Intrinsic::MutexNew));  // данные — мьютексу
    switch (rv.intrinsic) {
        case Intrinsic::CArenaMark: return "dp_carena_mark()";  // Р62
        case Intrinsic::TasksBegin: return "dp_tasks_begin()";  // Р70
        case Intrinsic::TasksEnd: return "dp_tasks_end(" + loc_ + ")";
        case Intrinsic::CArenaRelease: return "(dp_carena_release(" + ops[0] + "), (void)0)";
        case Intrinsic::Len: {
            const Type* t = op_type(rv.ops[0]);
            if (t->kind == TK::Array) return "((void)" + ops[0] + ", (int64_t)" + std::to_string(t->len) + ")";
            if (t->kind == TK::Pool) return "((int64_t)(" + ops[0] + ").m.len)";
            return "((int64_t)(" + ops[0] + ").len)";
        }
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
        case Intrinsic::AtomicNew: return ops[0];  // Atomic[T] — T в памяти (Р70)
        case Intrinsic::MutexNew: return "((" + ctype(rv.type) + "){dp_mtx_new(), " + ops[0] + "})";
        case Intrinsic::ChannelNew: return "dp_cnew__" + mangle(rv.type) + "((int64_t)" + ops[0] + ", " + loc_ + ")";
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
        case Intrinsic::MapOrder: return "dp_morder__" + mangle(op_type(rv.ops[0])) + "(&" + ops[0] + ")";
        case Intrinsic::PoolGet: return "dp_pget__" + mangle(op_type(rv.ops[0])) + "(&" + ops[0] + ", " + ops[1] + ")";
        case Intrinsic::PoolContains:
            return "dp_pcontains__" + mangle(op_type(rv.ops[0])) + "(&" + ops[0] + ", " + ops[1] + ")";
        case Intrinsic::PoolHandles: return "dp_phandles__" + mangle(op_type(rv.ops[0])) + "(&" + ops[0] + ")";
        case Intrinsic::PoolSlots: return "((int64_t)(" + ops[0] + ").m.nslots)";
        case Intrinsic::PoolLive: return "((" + ops[0] + ").m.live[" + ops[1] + "] != 0)";
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
        case Intrinsic::FileCreate: return "dp_file_create(" + ops[0] + ", " + loc_ + ")";
        case Intrinsic::FileOpen: return "dp_file_open(" + ops[0] + ", " + loc_ + ")";
        case Intrinsic::FileWrite: {  // Р71: срез []u8 — указатель и длина
            std::string d = fresh("dp_bytes");
            line(ctype(op_type(rv.ops[1])) + " " + d + " = " + ops[1] + ";");
            return "dp_file_write(" + ops[0] + ", " + d + ".ptr, " + d + ".len, " + loc_ + ")";
        }
        case Intrinsic::FileClose: return "dp_file_close(" + ops[0] + ", " + loc_ + ")";
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
            if (sema::c_borrowing(rv.type)) return borrowing_struct(rv);
            const sema::StructDef* sd = rv.type->sdef;
            if (sd->union_) {  // Р62: только заданное поле; остальное — нули
                std::string v = rv.variant > 0 ? operand(rv.ops[rv.variant - 1], true) : "";
                std::string r = fresh("dp_r");
                line(ctype(rv.type) + " " + r + ";");
                line("memset(&" + r + ", 0, sizeof " + r + ");");
                if (rv.variant > 0) line(r + ".f_" + sd->fields[rv.variant - 1].name + " = " + v + ";");
                return r;
            }
            std::vector<std::string> ops;
            for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + ";");
            line("memset(&" + r + ", 0, sizeof " + r + ");");
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
        case RvKind::MakeMap: case RvKind::MakePool: return "((" + ctype(rv.type) + "){0})";
        case RvKind::MakeList: {
            std::vector<std::string> ops;
            for (const Operand& o : rv.ops) ops.push_back(operand(o, true));
            std::string r = fresh("dp_r"), ec = ctype(rv.type->elem);
            line(ctype(rv.type) + " " + r + " = {0};");
            if (rv.type->kind == TK::Array) {  // Р62: на месте, без памяти
                for (size_t k = 0; k < ops.size(); k++) line(r + ".v[" + std::to_string(k) + "] = " + ops[k] + ";");
                return r;
            }
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
            if (niche(rv.type)) return v;
            std::string r = fresh("dp_r");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".has = true;");
            line(r + ".value = " + v + ";");
            return r;
        }
        case RvKind::MakeNone: return niche(rv.type) ? "((" + ctype(rv.type) + ")NULL)" : "((" + ctype(rv.type) + "){0})";
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
            bool arr = base.type->kind == TK::Array;  // Р62: элементы — v, длина — в типе
            std::string len = arr ? "(int64_t)" + std::to_string(base.type->len) : "(" + base.expr + ").len";
            std::string h = hi.empty() ? len : "(int64_t)" + hi;
            if (base.type->kind == TK::Str) return "dp_str_slice(" + base.expr + ", (int64_t)" + lo + ", " + h + ", " + loc_ + ")";
            std::string r = fresh("dp_r");
            line("dp_check_slice((int64_t)" + lo + ", " + h + ", " + len + ", " + loc_ + ");");
            line(ctype(rv.type) + " " + r + " = {0};");
            line(r + ".ptr = (" + base.expr + (arr ? ").v" : ").ptr") + " + (int64_t)" + lo + ";");
            line(r + ".len = " + h + " - (int64_t)" + lo + ";");
            return r;
        }
        case RvKind::CallPtr: {  // Р61: вызов C через указатель — как у extern fn
            const Type* ft = rv.ops[0].kind == OpKind::Const ? rv.ops[0].c.type : ir::place_type(p_, f_, rv.ops[0].place);
            std::string fp = fresh("dp_fp");
            line(ctype(ft) + " " + fp + " = " + operand(rv.ops[0], false) + ";");
            std::vector<CParam> params;
            for (const auto& [mode, t] : ft->sig->params) params.push_back(CParam{mode, t});
            return c_call(rv, params, ft->sig->ret, fp, "a C function pointer", "");
        }
        case RvKind::Call: {
            line("DP_CALL_CHECK(" + loc_ + ");");
            const Function& g = p_.fns[rv.fn];
            if (g.is_extern) return extern_call(rv, g);
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
        case RvKind::IsSome: {
            Lv o = place(rv.place);
            return niche(o.type) ? "((" + o.expr + ") != NULL)" : "((" + o.expr + ").has)";
        }
        case RvKind::IsOk: return "(!(" + place(rv.place).expr + ").is_err)";
        case RvKind::Zero:
            return is_composite(rv.type) || rv.type->kind == TK::Str || rv.type->kind == TK::PoolHandle
                       ? "((" + ctype(rv.type) + "){0})"
                       : "((" + ctype(rv.type) + ")0)";
        case RvKind::AddrOf: return "(&" + place(rv.place).expr + ")";  // Р62
        case RvKind::Spawn: {  // Р70: задача блока — в своём потоке
            std::string n = std::to_string(em_.task_id(rv));
            std::string env = fresh("dp_env");
            std::string init = "{";
            for (size_t i = 0; i < rv.args.size(); i++) {
                const Arg& a = rv.args[i];
                if (i) init += ", ";
                if (a.kind == Arg::Kind::Value) {
                    init += operand(a.op, true);
                    continue;
                }
                Lv lv = place(a.place);
                if (lv.byte) error("task borrows a str byte");
                init += "&" + lv.expr;
            }
            if (rv.args.empty()) init += "0";
            line("struct dp_tke" + n + " " + env + " = " + init + "};");
            line("dp_spawn(dp_tkb" + n + ", &" + env + ", sizeof " + env + ", " + loc_ + ");");
            return "((void)0)";
        }
        case RvKind::ParFor: {  // Р69: итерации — в пуле потоков runtime
            line("DP_CALL_CHECK(" + loc_ + ");");
            std::string n = std::to_string(em_.par_id(rv));
            const Type* t = rv.ops[0].kind == OpKind::Const ? rv.ops[0].c.type : ir::place_type(p_, f_, rv.ops[0].place);
            std::string lo = fresh("dp_lo"), hi = fresh("dp_hi"), env = fresh("dp_env");
            line(ctype(t) + " " + lo + " = " + operand(rv.ops[0], false) + ";");
            line(ctype(t) + " " + hi + " = " + operand(rv.ops[1], false) + ";");
            std::string init = "{" + lo;
            for (const Arg& a : rv.args) {
                Lv lv = place(a.place);
                if (lv.byte) error("parallel loop over a str byte");
                init += ", &" + lv.expr;
            }
            line("struct dp_pfe" + n + " " + env + " = " + init + "};");
            line("dp_parallel_for(" + hi + " > " + lo + " ? (uint64_t)" + hi + " - (uint64_t)" + lo + " : 0, dp_pfb" +
                 n + ", &" + env + ");");
            return "((void)0)";
        }
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
            } else if (in.canon) {  // Р56: канон до записи в память
                line(ctype(t) + " " + r + " = " + (t->kind == TK::F32 ? "dp_canon_f32(" : "dp_canon_f64(") + v + ");");
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

// Вызов функции C (Р59): str — копия с нулём в конце на время вызова,
// []T и &mut List[T] — указатель на элементы, &T и &mut T — указатель,
// ?H — дескриптор или NULL. Строка и дескриптор из C проверяются: NULL
// там, где он не ожидается, — ловушка.
// Заимствующая структура (Р62) — сразу в раскладке C: строка — копия с
// нулём в арене вызова, срез — указатель на элементы (срез строк — массив
// копий в арене), `?`-указатель — указатель или NULL; счётчик @count —
// длина среза (проверка диапазона и равенства длин у общего счётчика).
std::string FnEmitter::borrowing_struct(const Rvalue& rv) {
    const sema::StructDef* sd = rv.type->sdef;
    std::string r = fresh("dp_r");
    line(ctype(rv.type) + " " + r + ";");
    line("memset(&" + r + ", 0, sizeof " + r + ");");
    std::vector<std::string> lens(sd->fields.size());
    for (size_t k = 0; k < rv.ops.size() && k < sd->fields.size(); k++) {
        const Type* ft = sd->fields[k].type;
        const Operand& o = rv.ops[k];
        std::string v = operand(o, false);
        std::string f = r + ".f_" + sd->fields[k].name;
        if (ft->kind == TK::Str) {
            line(f + " = dp_carena_str(" + v + ", " + loc_ + ");");
            if (o.kind == OpKind::Move) line(drop_of(ft, v));  // временное отдало байты арене
        } else if (ft->kind == TK::Slice) {
            std::string sv = fresh("dp_sl");
            line(ctype(ft) + " " + sv + " = " + v + ";");
            lens[k] = sv + ".len";
            if (ft->elem->kind == TK::Str) {
                line(f + " = (const char**)dp_carena_alloc(" + sv + ".len, sizeof(const char*), " + loc_ + ");");
                line("for (int64_t i = 0; i < " + sv + ".len; i++) " + f + "[i] = dp_carena_str(" + sv + ".ptr[i], " +
                     loc_ + ");");
            } else {
                line(f + " = " + sv + ".len ? " + sv + ".ptr : NULL;");
            }
        } else {
            line(f + " = " + v + ";");
        }
    }
    std::vector<int> first(sd->fields.size(), -1);  // счётчик → первый срез с ним
    for (size_t k = 0; k < sd->fields.size(); k++) {
        int c = sd->fields[k].count;
        if (c < 0 || lens[k].empty()) continue;
        const sema::FieldDef& cf = sd->fields[static_cast<size_t>(c)];
        if (first[static_cast<size_t>(c)] >= 0) {
            const sema::FieldDef& other = sd->fields[static_cast<size_t>(first[static_cast<size_t>(c)])];
            line("if (" + lens[k] + " != " + lens[static_cast<size_t>(first[static_cast<size_t>(c)])] + ") dp_trapf(" + loc_ +
                 ", \"slices '%s' and '%s' share the count '%s' but have different lengths\", " + cstr(other.name) + ", " +
                 cstr(sd->fields[k].name) + ", " + cstr(cf.name) + ");");
            continue;
        }
        first[static_cast<size_t>(c)] = static_cast<int>(k);
        int b = sema::int_bits(cf.type);
        uint64_t max = sema::is_signed(cf.type) ? (b >= 64 ? INT64_MAX : (uint64_t{1} << (b - 1)) - 1)
                                                : (b >= 64 ? UINT64_MAX : (uint64_t{1} << b) - 1);
        line("if ((uint64_t)" + lens[k] + " > UINT64_C(" + std::to_string(max) + ")) dp_trapf(" + loc_ +
             ", \"a slice of %lld elements does not fit the count '%s'\", (long long)" + lens[k] + ", " + cstr(cf.name) + ");");
        line(r + ".f_" + cf.name + " = (" + ctype(cf.type) + ")" + lens[k] + ";");
    }
    return r;
}

std::string FnEmitter::extern_call(const Rvalue& rv, const Function& g) {
    std::vector<CParam> params;
    for (uint32_t i = 0; i < g.param_count; i++) params.push_back(CParam{Emitter::mode_of(g.locals[i]), g.locals[i].type});
    std::string what = "C function '" + g.c_name + "'";
    if (g.slot < 0) return c_call(rv, params, g.ret, Emitter::extern_name(g), what, g.c_name);
    // `@slot(N)` (Р63): первое слово объекта — адрес таблицы, вызов — её запись N.
    std::string obj = operand(rv.args[0].op, false);
    std::string table = fresh("dp_t"), n = std::to_string(g.slot);
    line("const dp_slot_fn* " + table + " = *(const dp_slot_fn* const*)" + obj + ";");
    line("if (!" + table + "[" + n + "]) dp_trapf(" + loc_ + ", \"%s: entry %d of the table is NULL\", " + cstr(what) + ", " + n +
         ");");
    std::string type = Emitter::c_result_type(g.ret) + " (*)(";
    for (uint32_t i = 0; i < g.param_count; i++) {
        type += (i ? ", " : "") + Emitter::c_param_type(Emitter::mode_of(g.locals[i]), g.locals[i].type);
    }
    type += std::string(g.param_count == 0 ? "void" : "") + ")";
    return c_call(rv, params, g.ret, "((" + type + ")" + table + "[" + n + "])", what, g.c_name);
}

// what — кто вызван, для ловушек: «C function 'SDL_Init'»; c_name — имя
// функции C для строки-результата (она бывает только у extern fn).
std::string FnEmitter::c_call(const Rvalue& rv, const std::vector<CParam>& params, const Type* ret,
                              const std::string& callee, const std::string& what, const std::string& c_name) {
    std::vector<std::string> args, frees;
    for (size_t i = 0; i < rv.args.size(); i++) {
        const Arg& a = rv.args[i];
        if (a.kind != Arg::Kind::Value) {
            Lv lv = place(a.place);
            // Пустой список — NULL: так C отличает «только узнать число» (Р62).
            args.push_back(params[i].type->kind == TK::List ? "((" + lv.expr + ").len ? (" + lv.expr + ").ptr : NULL)"
                                                            : "&" + lv.expr);
            continue;
        }
        std::string v = operand(a.op, false);
        const Type* t = params[i].type;
        if (t->kind == TK::Str) {
            std::string c = fresh("dp_c");
            line("char* " + c + " = dp_str_to_c(" + v + ", " + loc_ + ");");
            args.push_back(c);
            frees.push_back(c);
        } else if (t->kind == TK::Optional && t->elem->kind == TK::Str) {  // none — NULL
            std::string o = fresh("dp_o"), c = fresh("dp_c");
            line(ctype(t) + " " + o + " = " + v + ";");
            line("char* " + c + " = " + o + ".has ? dp_str_to_c(" + o + ".value, " + loc_ + ") : NULL;");
            args.push_back(c);
            frees.push_back(c);
        } else if (t->kind == TK::Slice && t->elem->kind == TK::Str) {  // Р62: массив строк C в арене
            std::string sv = fresh("dp_sl"), a = fresh("dp_a");
            line(ctype(t) + " " + sv + " = " + v + ";");
            line("const char** " + a + " = (const char**)dp_carena_alloc(" + sv + ".len, sizeof(const char*), " + loc_ + ");");
            line("for (int64_t i = 0; i < " + sv + ".len; i++) " + a + "[i] = dp_carena_str(" + sv + ".ptr[i], " + loc_ + ");");
            args.push_back(a);
        } else if (t->kind == TK::Slice) {  // пустой срез — NULL
            std::string sv = fresh("dp_sl");
            line(ctype(t) + " " + sv + " = " + v + ";");
            args.push_back("(" + sv + ".len ? " + sv + ".ptr : NULL)");
        } else {
            args.push_back(v);  // и ?H, ?extern fn, ?&T — указатель или NULL
        }
    }
    std::string call = callee + "(";
    for (size_t i = 0; i < args.size(); i++) call += (i ? ", " : "") + args[i];
    call += ")";
    const Type* t = ret;
    auto free_all = [&] {
        for (const std::string& c : frees) line("dp_free(" + c + ");");
    };
    if (t->kind == TK::Void) {
        line(call + ";");
        free_all();
        return "((void)0)";
    }
    std::string r = fresh("dp_r");
    line(Emitter::c_result_type(t) + " " + r + " = " + call + ";");
    free_all();
    if (t->kind == TK::Str) return "dp_str_from_c(" + r + ", " + cstr(c_name) + ", " + loc_ + ")";
    if (t->kind == TK::Handle || t->kind == TK::CFn) {
        line("if (!" + r + ") dp_trapf(" + loc_ + ", \"%s returned NULL\", " + cstr(what) + ");");
        return r;
    }
    if (t->kind == TK::Optional && t->elem->kind == TK::Str) {
        std::string o = fresh("dp_o");
        line(ctype(t) + " " + o + " = {0};");
        std::string value = "dp_str_from_c(" + r + ", " + cstr(c_name) + ", " + loc_ + ")";
        line("if (" + r + ") {\n            " + o + ".has = true;\n            " + o + ".value = " + value + ";\n        }");
        return o;
    }
    return r;
}

// Обёртка `@export` (Р61): значения C → значения dpl, вызов, результат →
// C. Строка — копия (её освобождает вызванная функция, как любой параметр
// по значению); NULL там, где dpl ждёт значение, — ловушка.
std::string Emitter::export_wrapper(const Function& f) {
    std::string out = export_head(f) + " {\n";
    std::string where = f.def ? loc(f.def->span) : "\"\"";
    std::string fname = cstr(f.c_name);
    std::vector<std::string> args;
    for (uint32_t i = 0; i < f.param_count; i++) {
        const ir::Local& l = f.locals[i];
        const Type* t = l.type;
        std::string a = "a" + std::to_string(i), pname = cstr(l.name);
        auto not_null = [&] {
            out += "    if (!" + a + ") dp_trapf(" + where + ", \"C passed NULL as '%s' of '%s'\", " + pname + ", " + fname + ");\n";
        };
        ast::ParamMode mode = mode_of(l);
        if (mode != ast::ParamMode::Value) {
            not_null();
            args.push_back("(" + ctype(t) + "*)" + a);
        } else if (t->kind == TK::Str) {
            std::string s = "s" + std::to_string(i);
            out += "    dp_str " + s + " = dp_str_from_c_arg(" + a + ", " + pname + ", " + fname + ", " + where + ");\n";
            args.push_back(s);
        } else if (t->kind == TK::Handle || t->kind == TK::CFn) {
            not_null();
            args.push_back(a);
        } else {  // и ?H, ?extern fn: NULL — none
            args.push_back(a);
        }
    }
    std::string call = fn_name(f) + "(";
    for (size_t i = 0; i < args.size(); i++) call += (i ? ", " : "") + args[i];
    call += ")";
    out += "    DP_CALL_CHECK(" + where + ");\n";
    if (f.ret->kind == TK::Void) return out + "    " + call + ";\n}\n\n";
    out += "    " + ctype(f.ret) + " r = " + call + ";\n";
    if (f.det && (f.ret->kind == TK::F32 || f.ret->kind == TK::F64)) {  // Р56: результат уходит из класса D в C
        return out + "    return " + (f.ret->kind == TK::F32 ? "dp_canon_f32(r)" : "dp_canon_f64(r)") + ";\n}\n\n";
    }
    return out + "    return r;\n}\n\n";
}

// Заголовок для хоста C (Р61): объявления `@export`, структуры `@c` и
// типы `extern fn(...)` их сигнатур. Дескриптор — void*.
std::string Emitter::header() {
    std::vector<const Type*> structs;
    std::set<const Type*> seen;
    std::map<const Type*, std::string> fn_names;
    std::map<std::string, int> name_count;
    std::string fn_defs;
    CType hct;
    std::function<void(const Type*)> need = [&](const Type* t) {
        if (!t || seen.count(t)) return;
        seen.insert(t);
        if (t->kind == TK::Optional || t->kind == TK::Array) need(t->elem);
        if (t->kind == TK::Struct) {
            for (const sema::FieldDef& fd : t->sdef->fields) need(fd.type);
            structs.push_back(t);
            name_count[t->sdef->name]++;
        }
        if (t->kind == TK::CFn) {
            for (const auto& p : t->sig->params) need(p.second);
            need(t->sig->ret);
            std::string n = "dp_fn" + std::to_string(fn_names.size() + 1);
            fn_names[t] = n;
            fn_defs += cfn_typedef(t, n, hct);
        }
    };
    // Имя dpl — слово C: с `_` в конце (`int` → `int_`).
    auto c_word = [](const std::string& s) { return is_c_keyword(s) ? s + "_" : s; };
    auto struct_name = [&](const Type* t) {
        const sema::StructDef* d = t->sdef;
        return c_word(name_count[d->name] > 1 ? d->module->name + "_" + d->name : d->name);
    };
    hct = [&](const Type* t) -> std::string {
        if (t->kind == TK::Optional && t->elem->kind == TK::CFn) t = t->elem;  // ?extern fn — указатель или NULL
        if (t->kind == TK::Struct) return (t->sdef->union_ ? "union " : "struct ") + struct_name(t);
        if (t->kind == TK::CFn) return fn_names.count(t) ? fn_names[t] : "void*";
        return ctype(t);  // числа, bool, дескриптор — void*
    };
    std::vector<const Function*> exported;
    for (const Function& f : p_.fns) {
        if (!f.exported) continue;
        exported.push_back(&f);
        for (uint32_t i = 0; i < f.param_count; i++) need(f.locals[i].type);
        need(f.ret);
    }
    std::string guard = "DPL_";
    for (char c : lib_name_) guard += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    guard += "_H";
    std::string out = "// Сгенерировано dpl (dpl build --lib) — не править.\n";
    out += "#ifndef " + guard + "\n#define " + guard + "\n\n#include <stdbool.h>\n#include <stdint.h>\n\n";
    out += "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
    // Сначала имена структур: их видят типы указателей на функции, затем
    // эти типы: их видят поля структур.
    for (const Type* t : structs) out += hct(t) + ";\n";
    if (!structs.empty()) out += "\n";
    if (!fn_defs.empty()) out += fn_defs + "\n";
    for (const Type* t : structs) {
        out += hct(t) + " {\n";
        for (const sema::FieldDef& fd : t->sdef->fields) {
            const Type* ft = fd.type;
            std::string dims;  // массив C (Р62): T name[N][M]
            while (ft->kind == TK::Array) {
                dims += "[" + std::to_string(ft->len) + "]";
                ft = ft->elem;
            }
            out += "    " + hct(ft) + " " + c_word(fd.name) + dims + ";\n";
        }
        out += "};\n\n";
    }
    for (const Function* f : exported) {
        out += c_result_type(f->ret, hct) + " " + f->c_name + "(";
        std::set<std::string> used;
        for (uint32_t i = 0; i < f->param_count; i++) {
            // `_` и повтор имени C не примет: тогда — a<номер>.
            std::string pn = c_word(f->locals[i].name);
            if (pn.empty() || pn == "_" || !used.insert(pn).second) pn = "a" + std::to_string(i);
            out += (i ? ", " : "") + c_param_type(mode_of(f->locals[i]), f->locals[i].type, hct) + " " + pn;
        }
        out += std::string(f->param_count == 0 ? "void" : "") + ");\n";
    }
    out += "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n";
    return out;
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

CResult emit_c_lib(const ir::Program& p, const SourceMap& sources, const std::string& name) {
    return Emitter(p, sources, nullptr, true, name).run();
}

}  // namespace dp::backend_c
