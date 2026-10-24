// Текстовая запись Core IR.
#include "ir/dump.h"

#include <cstdio>

#include "support/json.h"

namespace dp::ir {

using sema::TK;

namespace {

class Dumper {
public:
    Dumper(const Program& p, const Function& f, const SourceMap& sources) : p_(p), f_(f), sources_(sources) {}

    std::string run() {
        out_ += std::string(f_.is_extern ? "extern fn " : "fn ") + f_.name + "(";
        for (uint32_t i = 0; i < f_.param_count; i++) {
            const Local& l = f_.locals[i];
            if (i) out_ += ", ";
            out_ += "%" + std::to_string(i) + " " + (l.name.empty() ? "_" : l.name) + ": ";
            if (l.kind == LocalKind::ParamRef) out_ += "&";
            if (l.kind == LocalKind::ParamMut) out_ += "&mut ";
            out_ += type(l.type);
        }
        out_ += ")";
        if (f_.ret && f_.ret->kind != TK::Void) out_ += " " + type(f_.ret);
        if (f_.det) out_ += " det";
        if (f_.exported) out_ += " export " + f_.c_name;
        if (f_.is_extern) {
            out_ += " = " + f_.c_name + "\n";
            return out_;
        }
        out_ += "\n";
        for (size_t i = f_.param_count; i < f_.locals.size(); i++) {
            const Local& l = f_.locals[i];
            out_ += l.kind == LocalKind::Var ? "  var %" : "  tmp %";
            out_ += std::to_string(i);
            if (!l.name.empty()) out_ += " " + l.name;
            out_ += ": " + type(l.type) + "\n";
        }
        for (size_t b = 0; b < f_.blocks.size(); b++) {
            out_ += "bb" + std::to_string(b) + ":\n";
            for (const Inst& i : f_.blocks[b].insts) inst(i);
            term(f_.blocks[b].term);
        }
        return out_;
    }

private:
    const Program& p_;
    const Function& f_;
    const SourceMap& sources_;
    std::string out_;

    std::string type(const Type* t) { return t ? sema::type_name(t, f_.def ? f_.def->module : nullptr) : "?"; }

    std::string at(Span s) {
        if (s.end == 0 && s.begin == 0) return "";
        if (s.file >= sources_.size()) return "";
        LineCol lc = sources_.file(s.file).line_col(s.begin);
        return "  @" + std::to_string(lc.line) + ":" + std::to_string(lc.col);
    }

    std::string place(const Place& pl) {
        std::string s = "%" + std::to_string(pl.local);
        const Type* t = pl.local < f_.locals.size() ? f_.locals[pl.local].type : nullptr;
        for (const Proj& pr : pl.proj) {
            switch (pr.kind) {
                case ProjKind::Field:
                    s += "." + (t && t->kind == TK::Struct ? t->sdef->fields[pr.index].name : std::to_string(pr.index));
                    if (t && t->kind == TK::Struct) t = t->sdef->fields[pr.index].type;
                    break;
                case ProjKind::Index:
                    s += "[%" + std::to_string(pr.index) + "]";
                    if (t) t = t->elem;
                    break;
                case ProjKind::Payload: {
                    if (!t || t->kind != TK::Enum) {
                        s += ".?";
                        break;
                    }
                    const sema::VariantDef& v = t->edef->variants[pr.index];
                    s += "." + v.name + ".";
                    if (v.form == ast::VariantForm::Record) {
                        s += v.fields[pr.sub].name;
                        t = v.fields[pr.sub].type;
                    } else {
                        s += std::to_string(pr.sub);
                        t = v.types[pr.sub];
                    }
                    break;
                }
                case ProjKind::Unwrap: s += ".some"; if (t) t = t->elem; break;
                case ProjKind::ResultValue: s += ".ok"; if (t) t = t->elem; break;
                case ProjKind::ResultError: s += ".err"; if (t) t = t->err; break;
                case ProjKind::MapValue: s += "{%" + std::to_string(pr.index) + "}"; if (t) t = t->elem; break;
                case ProjKind::MapKeyAt: s += ".key[%" + std::to_string(pr.index) + "]"; if (t) t = t->key; break;
                case ProjKind::MapValueAt: s += ".value[%" + std::to_string(pr.index) + "]"; if (t) t = t->elem; break;
            }
        }
        return s;
    }

    std::string constant(const Const& c) {
        switch (c.kind) {
            case Const::Kind::Int: {
                bool neg = sema::is_signed(c.type) && static_cast<int64_t>(c.i) < 0;
                return (neg ? std::to_string(static_cast<int64_t>(c.i)) : std::to_string(c.i)) + ":" + type(c.type);
            }
            case Const::Kind::Float: {
                char buf[40];
                std::snprintf(buf, sizeof buf, "%.17g", c.f);
                return std::string(buf) + ":" + type(c.type);
            }
            case Const::Kind::Bool: return c.b ? "true" : "false";
            case Const::Kind::Str: {
                std::string s;
                json_string(s, c.str < p_.strings.size() ? p_.strings[c.str] : "?");
                return s;
            }
            case Const::Kind::Fn: return "fn " + (c.fn < p_.fns.size() ? p_.fns[c.fn].name : std::string("?"));
        }
        return "?";
    }

    std::string operand(const Operand& o) {
        switch (o.kind) {
            case OpKind::Copy: return "copy " + place(o.place);
            case OpKind::Move: return "move " + place(o.place);
            case OpKind::Const: return constant(o.c);
        }
        return "?";
    }

    std::string ops(const std::vector<Operand>& os, size_t from = 0) {
        std::string s;
        for (size_t i = from; i < os.size(); i++) s += (i > from ? ", " : "") + operand(os[i]);
        return s;
    }

    static const char* bin_name(BinKind k) {
        switch (k) {
            case BinKind::Add: return "add";
            case BinKind::Sub: return "sub";
            case BinKind::Mul: return "mul";
            case BinKind::Div: return "div";
            case BinKind::Rem: return "rem";
            case BinKind::AddWrap: return "add.wrap";
            case BinKind::SubWrap: return "sub.wrap";
            case BinKind::MulWrap: return "mul.wrap";
            case BinKind::BitAnd: return "and";
            case BinKind::BitOr: return "or";
            case BinKind::BitXor: return "xor";
            case BinKind::Shl: return "shl";
            case BinKind::Shr: return "shr";
            case BinKind::Eq: return "eq";
            case BinKind::Ne: return "ne";
            case BinKind::Lt: return "lt";
            case BinKind::Le: return "le";
            case BinKind::Gt: return "gt";
            case BinKind::Ge: return "ge";
            case BinKind::Concat: return "concat";
        }
        return "?";
    }

    static const char* intrinsic_name(Intrinsic k) {
        static constexpr const char* names[] = {
            "len",  "repeat", "min",   "max",    "abs",    "parse_int", "is_digit", "is_alpha",
            "is_space", "is_alnum", "print", "println", "eprintln", "push", "pop", "insert",
            "remove", "clear", "sort", "reserve", "contains", "trim", "find", "starts_with", "split",
            "__args_len", "__arg", "__read_file", "checked_add", "checked_sub", "checked_mul", "checked_div",
            "checked_neg", "map_set", "map_get", "map_contains", "map_remove", "map_clear", "map_keys",
            "map_order", "map_reserve",
            "c_arena_mark", "c_arena_release", "probe",
        };
        return names[static_cast<size_t>(k)];
    }

    std::string rvalue(const Rvalue& rv) {
        switch (rv.kind) {
            case RvKind::Use: return operand(rv.ops[0]);
            case RvKind::Binary: return std::string(bin_name(rv.bin)) + " " + ops(rv.ops);
            case RvKind::Unary:
                return std::string(rv.un == UnKind::Neg     ? "neg "
                                   : rv.un == UnKind::Not   ? "not "
                                   : rv.un == UnKind::Canon ? "canon "
                                                            : "bitnot ") +
                       ops(rv.ops);
            case RvKind::Cast: {
                const char* k = rv.cast == CastKind::Widen ? "widen" : rv.cast == CastKind::Check ? "check"
                              : rv.cast == CastKind::Wrap ? "wrap" : rv.cast == CastKind::RefUp ? "ref_up" : "str";
                return std::string(k) + " " + ops(rv.ops) + " to " + type(rv.type);
            }
            case RvKind::MakeStruct: return type(rv.type) + " { " + ops(rv.ops) + " }";
            case RvKind::MakeVariant: {
                std::string s = type(rv.type) + "." + rv.type->edef->variants[rv.variant].name;
                if (!rv.ops.empty()) s += "(" + ops(rv.ops) + ")";
                return s;
            }
            case RvKind::MakeList: return type(rv.type) + " [" + ops(rv.ops) + "]";
            case RvKind::MakeMap: return type(rv.type) + " []";
            case RvKind::MakeSome: return "some " + ops(rv.ops);
            case RvKind::MakeNone: return "none " + type(rv.type);
            case RvKind::MakeOk: return rv.ops.empty() ? "ok " + type(rv.type) : "ok " + ops(rv.ops);
            case RvKind::MakeErr: return "err " + ops(rv.ops) + " " + type(rv.type);
            case RvKind::SliceOf: {
                std::string s = "slice " + place(rv.place) + "[" + operand(rv.ops[0]) + "..";
                if (rv.ops.size() > 1) s += operand(rv.ops[1]);
                return s + "]";
            }
            case RvKind::Call: case RvKind::CallPtr: {
                std::string s = rv.kind == RvKind::CallPtr ? "call_ptr " + ops(rv.ops) + "("
                                                           : "call " + (rv.fn < p_.fns.size() ? p_.fns[rv.fn].name : "?") + "(";
                for (size_t i = 0; i < rv.args.size(); i++) {
                    const Arg& a = rv.args[i];
                    if (i) s += ", ";
                    if (a.kind == Arg::Kind::Value) s += operand(a.op);
                    else s += (a.kind == Arg::Kind::Ref ? "&" : "&mut ") + place(a.place);
                }
                return s + ")";
            }
            case RvKind::Intrinsic: {
                std::string s = std::string(intrinsic_name(rv.intrinsic)) + "(";
                if (rv.has_place) s += "&mut " + place(rv.place) + (rv.ops.empty() ? "" : ", ");
                return s + ops(rv.ops) + ")";
            }
            case RvKind::Tag: return "tag " + place(rv.place);
            case RvKind::IsSome: return "is_some " + place(rv.place);
            case RvKind::IsOk: return "is_ok " + place(rv.place);
            case RvKind::Zero: return "zero " + type(rv.type);
            case RvKind::AddrOf: return "addr " + place(rv.place);
        }
        return "?";
    }

    void line(const std::string& text, Span s) { out_ += "  " + text + at(s) + "\n"; }

    void inst(const Inst& i) {
        switch (i.kind) {
            case InstKind::Assign:  // Р56: canon(…) — канон до записи
                line(place(i.dst) + " = " + (i.canon ? "canon(" + rvalue(i.rv) + ")" : rvalue(i.rv)), i.span);
                break;
            case InstKind::Eval: line("eval " + rvalue(i.rv), i.span); break;
            case InstKind::Drop: line("drop " + place(i.dst), i.span); break;
        }
    }

    void term(const Term& t) {
        switch (t.kind) {
            case TermKind::None: line("<no terminator>", t.span); break;
            case TermKind::Jump: line("jump bb" + std::to_string(t.then_b), t.span); break;
            case TermKind::Branch:
                line("branch " + operand(t.cond) + ", bb" + std::to_string(t.then_b) + ", bb" + std::to_string(t.else_b),
                     t.span);
                break;
            case TermKind::Return: line(t.has_value ? "return " + operand(t.value) : "return", t.span); break;
            case TermKind::Trap: {
                std::string m;
                json_string(m, t.message);
                line("trap " + m, t.span);
                break;
            }
            case TermKind::Unreachable: line("unreachable", t.span); break;
        }
    }
};

}  // namespace

std::string dump_function(const Program& p, const Function& f, const SourceMap& sources) {
    return Dumper(p, f, sources).run();
}

std::string dump_program(const Program& p, const SourceMap& sources) {
    std::string out;
    for (size_t i = 0; i < p.fns.size(); i++) {
        if (i) out += "\n";
        out += dump_function(p, p.fns[i], sources);
    }
    return out;
}

}  // namespace dp::ir
