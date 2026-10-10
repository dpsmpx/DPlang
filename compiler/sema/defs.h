// Объявления программы и модули (docs/spec/core.md, раздел 3).
//
// Модуль проверяется как функция с явными входами (Р43): его AST и
// сводки модулей, которые он импортирует. Сводка — открытые объявления с
// разрешёнными типами (Def ниже с pub = true); тела функций в неё не
// входят, поэтому правка тела не меняет сводку (Р14, Р41).
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sema/types.h"
#include "support/source.h"
#include "syntax/ast.h"

namespace dp::sema {

struct ModuleInfo;

struct FieldDef {
    std::string name;
    Span span;
    const Type* type = nullptr;
    const ast::Expr* default_value = nullptr;
};

struct StructDef {
    std::string name;
    const ModuleInfo* module = nullptr;
    bool pub = false;
    bool c = false;       // `@c`: раскладка C (Р59)
    bool linear = false;  // `@linear`: значение закрывается ровно один раз (Р57)
    Span span;
    std::vector<FieldDef> fields;
    const ast::StructItem* node = nullptr;
    const FieldDef* field(const std::string& n) const {
        for (const FieldDef& f : fields) {
            if (f.name == n) return &f;
        }
        return nullptr;
    }
};

struct VariantDef {
    std::string name;
    Span span;
    ast::VariantForm form = ast::VariantForm::Unit;
    std::vector<const Type*> types;  // Tuple
    std::vector<FieldDef> fields;    // Record
};

struct EnumDef {
    std::string name;
    const ModuleInfo* module = nullptr;
    bool pub = false;
    Span span;
    std::vector<VariantDef> variants;
    const ast::EnumItem* node = nullptr;
    // Номер варианта; -1 — нет такого.
    int find(const std::string& n) const {
        for (size_t i = 0; i < variants.size(); i++) {
            if (variants[i].name == n) return static_cast<int>(i);
        }
        return -1;
    }
};

struct ParamDef {
    std::string name;
    ast::ParamMode mode = ast::ParamMode::Value;
    const Type* type = nullptr;
    Span span;
};

struct FnDef {
    std::string name;
    const ModuleInfo* module = nullptr;
    bool pub = false;
    bool det = false;   // @det (Р12)
    bool test = false;  // @test (Р50)
    bool is_extern = false;  // `extern fn` — функция C (Р59): тела нет
    std::string link;        // `@link("lib")` — библиотека C; пусто — библиотека C по умолчанию
    Span span;
    std::vector<ParamDef> params;
    const Type* ret = nullptr;  // Void — без результата
    const ast::FnItem* node = nullptr;
    bool infer_error = false;   // `!T` без типа ошибки: тип выводится из тела (Р41)
    uint32_t local_count = 0;   // локальных переменных в теле (после проверки)
};

// Значение константы, вычисленное при проверке (Р42: константные выражения).
struct ConstValue {
    enum class Kind : uint8_t { None, Int, Float, Bool, Str, List } kind = Kind::None;
    uint64_t i = 0;  // целое: биты значения (знаковое — в дополнительном коде)
    double f = 0;
    bool b = false;
    std::string s;
    std::vector<ConstValue> items;  // List
};

struct ConstDef {
    std::string name;
    const ModuleInfo* module = nullptr;
    bool pub = false;
    Span span;
    const Type* type = nullptr;
    const ast::ConstItem* node = nullptr;
    ConstValue value;
    uint8_t state = 0;  // 0 — не вычислена, 1 — вычисляется, 2 — готова
};

// Локальная переменная функции: параметр, let/var, переменная цикла,
// связанное имя образца, `if let`.
struct Local {
    std::string name;
    Span span;
    const Type* type = nullptr;
    bool mutable_ = false;   // var, &mut-параметр, переменная `for … in &mut`
    bool is_ref = false;     // ссылка (Р44): параметр &T/&mut T, переменная `for … in &`
    bool is_param = false;
    Local* alias_of = nullptr;  // переменная `for x in &xs` — элемент xs: корень xs (Р44)
    uint32_t id = 0;         // номер в функции
};

// `extern type Name` — непрозрачный тип C (Р59).
struct HandleDef {
    std::string name;
    const ModuleInfo* module = nullptr;
    bool pub = false;
    Span span;
};

enum class SymKind : uint8_t { Fn, Struct, Enum, Const, Module, Handle };

struct Symbol {
    SymKind kind = SymKind::Fn;
    FnDef* fn = nullptr;
    StructDef* st = nullptr;
    EnumDef* en = nullptr;
    ConstDef* cn = nullptr;
    ModuleInfo* mod = nullptr;
    HandleDef* hd = nullptr;
    Span span;  // объявление или import
};

struct ModuleInfo {
    std::string name;
    std::string path;
    FileId file = 0;
    std::unique_ptr<ast::Module> ast;
    std::vector<ModuleInfo*> imports;
    std::map<std::string, Symbol> scope;  // свои объявления и имена импортированных модулей
    bool ok = true;                       // разобран без ошибок
    bool stdlib = false;                  // модуль стандартной библиотеки: ему видны встроенные `__…` (Р51)
    bool det = false;                     // класс D: импортирован с `@det` хотя бы одним модулем (Р55)
};

// Функция принадлежит классу D: помечена `@det` или её модуль — класса D (Р55).
inline bool is_det(const FnDef& f) { return f.det || (f.module && f.module->det); }

// Программа одного запуска: модули в порядке зависимостей (сначала те,
// что импортируют другие), объявления и таблица типов.
struct Program {
    std::vector<std::unique_ptr<ModuleInfo>> modules;
    ModuleInfo* root = nullptr;
    bool load_ok = true;  // все модули найдены и разобраны, циклов нет
    TypeTable types;
    std::deque<StructDef> structs;
    std::deque<EnumDef> enums;
    std::deque<FnDef> fns;
    std::deque<ConstDef> consts;
    std::deque<HandleDef> handles;
    std::deque<Local> locals;
};

}  // namespace dp::sema
