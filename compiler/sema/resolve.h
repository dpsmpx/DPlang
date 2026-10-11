// Разрешение имён верхнего уровня и типов в записи (docs/spec/core.md,
// разделы 3 и 4) — общее для сбора объявлений и проверки тел.
#pragma once

#include <string>

#include "sema/defs.h"
#include "support/diag.h"

namespace dp::sema {

class Resolver {
public:
    Resolver(Program& prog, ModuleInfo& m, Diagnostics& diags) : prog_(prog), m_(m), diags_(diags) {}

    // Тип по записи; ставит node->resolved. Ошибки — в диагностике, тип — Error.
    const Type* type(ast::Type* node, bool allow_void = false);

    // Имя верхнего уровня текущего модуля (своё объявление или модуль); nullptr — нет.
    const Symbol* lookup(const std::string& name) const;

    // Открытый член модуля mod. Нет или закрыт — nullptr и сообщение у span.
    const Symbol* member(const ModuleInfo& mod, const std::string& name, Span span);

    Program& prog() { return prog_; }
    void check_cfn(const ast::FnType* node, const Type* t);
    ModuleInfo& module() { return m_; }

private:
    Program& prog_;
    ModuleInfo& m_;
    Diagnostics& diags_;
};

// Типы параметров и результата `extern fn(...)` — с видом в C (Р61, E0326).
void check_cfn(const ast::FnType* node, const Type* t, const ModuleInfo* m, Diagnostics& diags);

}  // namespace dp::sema
