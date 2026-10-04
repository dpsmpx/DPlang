// Сводка модуля (Р14): открытые объявления с разрешёнными типами, в
// записи языка и в порядке объявления. Тел функций и значений констант
// в ней нет — правка тела сводку не меняет.
#include "sema/sema.h"

namespace dp::sema {

namespace {

std::string mode_text(ast::ParamMode m) {
    return m == ast::ParamMode::Mut ? "&mut " : m == ast::ParamMode::Ref ? "&" : "";
}

std::string fields_text(const std::vector<FieldDef>& fields, const ModuleInfo* m) {
    std::string s;
    for (size_t i = 0; i < fields.size(); i++) {
        s += (i ? ", " : "") + fields[i].name + ": " + type_name(fields[i].type, m);
        if (fields[i].default_value) s += " = ...";
    }
    return s;
}

}  // namespace

std::string summary_text(const ModuleInfo& m) {
    std::string out = "module " + m.name + "\n";
    for (const ast::Item* it : m.ast->items) {
        if (!it->pub) continue;
        auto found = m.scope.find(it->name);
        if (found == m.scope.end()) continue;
        const Symbol& s = found->second;
        switch (it->kind) {
            case ast::ItemKind::Fn: {
                if (!s.fn || s.fn->node != it) break;
                const FnDef& f = *s.fn;
                out += "  ";
                if (f.det) out += "@det ";
                out += "pub fn " + f.name + "(";
                for (size_t i = 0; i < f.params.size(); i++) {
                    const ParamDef& p = f.params[i];
                    out += (i ? ", " : "") + p.name + ": " + mode_text(p.mode) + type_name(p.type, &m);
                }
                out += ")";
                if (f.ret && f.ret->kind != TK::Void) out += " " + type_name(f.ret, &m);
                out += "\n";
                break;
            }
            case ast::ItemKind::Struct:
                if (!s.st || s.st->node != it) break;
                out += "  pub struct " + s.st->name + " { " + fields_text(s.st->fields, &m) + " }\n";
                break;
            case ast::ItemKind::Enum: {
                if (!s.en || s.en->node != it) break;
                out += "  pub enum " + s.en->name + " {";
                for (size_t i = 0; i < s.en->variants.size(); i++) {
                    const VariantDef& v = s.en->variants[i];
                    out += (i ? ", " : " ") + v.name;
                    if (v.form == ast::VariantForm::Tuple) {
                        out += "(";
                        for (size_t k = 0; k < v.types.size(); k++) out += (k ? ", " : "") + type_name(v.types[k], &m);
                        out += ")";
                    } else if (v.form == ast::VariantForm::Record) {
                        out += " { " + fields_text(v.fields, &m) + " }";
                    }
                }
                out += " }\n";
                break;
            }
            case ast::ItemKind::Const:
                if (!s.cn || s.cn->node != it) break;
                out += "  pub const " + s.cn->name + ": " + type_name(s.cn->type, &m) + "\n";
                break;
            default: break;
        }
    }
    return out;
}

}  // namespace dp::sema
