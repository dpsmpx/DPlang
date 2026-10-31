// Общие функции Core IR: типы мест и операндов, владение памятью.
#include "ir/ir.h"

namespace dp::ir {

using sema::TK;

const Type* place_type(const Program& prog, const Function& f, const Place& p) {
    if (p.local >= f.locals.size()) return nullptr;
    const Type* t = f.locals[p.local].type;
    for (size_t k = 0; k < p.proj.size(); k++) {
        const Proj& pr = p.proj[k];
        if (!t) return nullptr;
        switch (pr.kind) {
            case ProjKind::Field:
                if (t->kind == TK::PoolHandle && pr.index < 2) {  // Р67: index, generation
                    t = prog.types->prim(TK::U32);
                    break;
                }
                if (t->kind != TK::Struct || pr.index >= t->sdef->fields.size()) return nullptr;
                t = t->sdef->fields[pr.index].type;
                break;
            case ProjKind::Index:
                if (pr.index >= f.locals.size() || f.locals[pr.index].type->kind != TK::I64) return nullptr;
                if (t->kind == TK::List || t->kind == TK::Slice || t->kind == TK::Array) {
                    t = t->elem;
                } else if (t->kind == TK::Str && k + 1 == p.proj.size()) {
                    t = prog.types->prim(TK::U8);  // байт строки — только последним
                } else {
                    return nullptr;
                }
                break;
            case ProjKind::Payload: {
                if (t->kind != TK::Enum || pr.index >= t->edef->variants.size()) return nullptr;
                const sema::VariantDef& v = t->edef->variants[pr.index];
                if (v.form == ast::VariantForm::Tuple && pr.sub < v.types.size()) t = v.types[pr.sub];
                else if (v.form == ast::VariantForm::Record && pr.sub < v.fields.size()) t = v.fields[pr.sub].type;
                else return nullptr;
                break;
            }
            case ProjKind::Unwrap:
                if (t->kind != TK::Optional) return nullptr;
                t = t->elem;
                break;
            case ProjKind::ResultValue:
                if (t->kind != TK::Result || t->elem->kind == TK::Void) return nullptr;
                t = t->elem;
                break;
            case ProjKind::ResultError:
                if (t->kind != TK::Result || !t->err) return nullptr;
                t = t->err;
                break;
            case ProjKind::MapValue:
                if (t->kind != TK::Map || pr.index >= f.locals.size() || f.locals[pr.index].type != t->key) {
                    return nullptr;
                }
                t = t->elem;
                break;
            case ProjKind::PoolValue: {
                const Type* h = pr.index < f.locals.size() ? f.locals[pr.index].type : nullptr;
                if (t->kind != TK::Pool || !h || h->kind != TK::PoolHandle || h->elem != t->elem) return nullptr;
                t = t->elem;
                break;
            }
            case ProjKind::PoolSlot:
                if (t->kind != TK::Pool || pr.index >= f.locals.size() || f.locals[pr.index].type->kind != TK::I64) {
                    return nullptr;
                }
                t = t->elem;
                break;
            case ProjKind::MapKeyAt: case ProjKind::MapValueAt:
                if (t->kind != TK::Map || pr.index >= f.locals.size() || f.locals[pr.index].type->kind != TK::I64) {
                    return nullptr;
                }
                t = pr.kind == ProjKind::MapKeyAt ? t->key : t->elem;
                break;
        }
    }
    return t;
}

bool needs_drop(const Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TK::Str: case TK::List: case TK::Map: case TK::Pool: return true;
        case TK::Optional: return needs_drop(t->elem);
        case TK::Result: return needs_drop(t->elem) || needs_drop(t->err);
        case TK::Struct:
            // Р62: заимствующая структура — раскладка C, её строки и массивы — в арене вызова.
            if (sema::c_borrowing(t)) return false;
            for (const sema::FieldDef& f : t->sdef->fields) {
                if (needs_drop(f.type)) return true;
            }
            return false;
        case TK::Enum:
            for (const sema::VariantDef& v : t->edef->variants) {
                for (const Type* x : v.types) {
                    if (needs_drop(x)) return true;
                }
                for (const sema::FieldDef& f : v.fields) {
                    if (needs_drop(f.type)) return true;
                }
            }
            return false;
        default: return false;
    }
}

const Type* operand_type(const Program& prog, const Function& f, const Operand& op) {
    if (op.kind == OpKind::Const) return op.c.type;
    return place_type(prog, f, op.place);
}

}  // namespace dp::ir
