// Загрузка модулей программы (docs/spec/core.md, раздел 3; Р46).
#include <cstdlib>
#include <map>

#include "sema/sema.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

namespace dp::sema {

namespace {

std::string dir_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

std::string stem_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.rfind('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}

bool valid_module_name(const std::string& name) {
    if (name.empty()) return false;
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    // Ключевые слова — те же, что знает лексер.
    SourceFile probe("<name>", name);
    Diagnostics ignore;
    std::vector<Token> toks = lex(probe, 0, ignore);
    return toks.size() == 2 && toks[0].kind == TokKind::Ident;
}

class Loader {
public:
    Loader(Program& prog, const LoadOptions& opt, SourceMap& sources, Diagnostics& diags)
        : prog_(prog), opt_(opt), sources_(sources), diags_(diags) {}

    ModuleInfo* load_root(const std::string& path) {
        root_dir_ = dir_of(path);
        std::string text;
        if (!read(path, text)) {
            diags_.error(Code::E0001, "cannot read source file '" + path + "'");
            return nullptr;
        }
        return load(stem_of(path), path, std::move(text), path);
    }

private:
    Program& prog_;
    const LoadOptions& opt_;
    SourceMap& sources_;
    Diagnostics& diags_;
    std::string root_dir_;
    std::map<std::string, ModuleInfo*> by_name_;
    std::map<std::string, bool> done_;     // false — модуль ещё загружается (на стеке)
    std::vector<std::string> stack_;

    bool read(const std::string& path, std::string& out) const {
        return opt_.reader ? opt_.reader->read(path, out) : read_file(path, out);
    }

    // shown — путь в сообщениях, местах ловушек и `#line`; у модулей
    // библиотеки и привязок — без каталога установки: сборка не зависит от
    // него (docs/policy/DETERMINISM.md, раздел 6).
    ModuleInfo* load(const std::string& name, const std::string& path, std::string text, const std::string& shown) {
        auto owned = std::make_unique<ModuleInfo>();
        ModuleInfo* m = owned.get();
        m->name = name;
        m->path = path;
        m->file = sources_.add(shown, std::move(text));
        size_t errors_before = diags_.error_count();
        ParseOptions po;
        po.max_nesting = opt_.max_nesting;
        m->ast = parse_module(sources_.file(m->file), m->file, name, diags_, po);
        m->ok = diags_.error_count() == errors_before;
        if (!m->ok) prog_.load_ok = false;
        if (!valid_module_name(name)) {
            diags_.error(Code::E0317, "module file name '" + path + "' is not a valid module name");
            prog_.load_ok = false;
        }
        by_name_[name] = m;
        done_[name] = false;
        stack_.push_back(name);
        std::map<std::string, Span> seen;
        for (ast::Item* it : m->ast->items) {
            auto* im = ast::as<ast::ImportItem>(it);
            if (!im || im->name.empty()) continue;
            if (seen.count(im->name)) {
                diags_.error(Code::E0303, im->name_span, "module '" + im->name + "' is imported twice");
                continue;
            }
            seen[im->name] = im->name_span;
            if (ModuleInfo* dep = import(im->name, im->name_span)) m->imports.push_back(dep);
        }
        stack_.pop_back();
        done_[name] = true;
        prog_.modules.push_back(std::move(owned));  // после зависимостей — порядок зависимостей
        return m;
    }

    ModuleInfo* import(const std::string& name, Span span) {
        auto it = by_name_.find(name);
        if (it != by_name_.end()) {
            if (!done_[name]) {
                std::string chain;
                bool on = false;
                for (const std::string& s : stack_) {
                    if (s == name) on = true;
                    if (on) chain += s + " -> ";
                }
                diags_.error(Code::E0301, span, "import cycle: " + chain + name);
                prog_.load_ok = false;
                return nullptr;
            }
            return it->second;
        }
        std::vector<std::string> places = {root_dir_ + name + ".dpm"};
        std::vector<std::string> shown = places;
        size_t lib = std::string::npos;  // номер места стандартной библиотеки
        if (!opt_.stdlib_dir.empty()) {
            lib = places.size();
            places.push_back(opt_.stdlib_dir + "/" + name + ".dpm");
            shown.push_back("stdlib/" + name + ".dpm");
        }
        if (!opt_.bindings_dir.empty()) {
            places.push_back(opt_.bindings_dir + "/" + name + "/" + name + ".dpm");
            shown.push_back("bindings/" + name + "/" + name + ".dpm");
        }
        for (size_t i = 0; i < places.size(); i++) {
            std::string text;
            if (!read(places[i], text)) continue;
            ModuleInfo* m = load(name, places[i], std::move(text), shown[i]);
            if (m && i == lib) m->stdlib = true;  // найден в стандартной библиотеке
            return m;
        }
        // Пути библиотеки и привязок зависят от установки — в сообщении
        // только их названия.
        std::string looked = places[0];
        if (!opt_.stdlib_dir.empty()) looked += opt_.bindings_dir.empty() ? " and the standard library" : ", the standard library";
        if (!opt_.bindings_dir.empty()) looked += " and the bindings";
        diags_.error(Code::E0302, span, "cannot find module '" + name + "' (looked in " + looked + ")");
        prog_.load_ok = false;
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<Program> load_program(const std::string& root_path, const LoadOptions& opt, SourceMap& sources,
                                      Diagnostics& diags) {
    auto prog = std::make_unique<Program>();
    Loader loader(*prog, opt, sources, diags);
    prog->root = loader.load_root(root_path);
    if (!prog->root) return nullptr;
    // Главный модуль из каталога библиотеки (проверка самой библиотеки) —
    // тоже модуль библиотеки. Только у файлов CLI: у чтения из памяти
    // (worker в песочнице) каталога нет.
    if (!opt.reader && !opt.stdlib_dir.empty()) {
        char* lib = realpath(opt.stdlib_dir.c_str(), nullptr);
        char* file = realpath(root_path.c_str(), nullptr);
        if (lib && file) {
            std::string d(file), l(lib);
            size_t slash = d.find_last_of('/');
            prog->root->stdlib = slash != std::string::npos && d.substr(0, slash) == l;
        }
        std::free(lib);
        std::free(file);
    }
    return prog;
}

}  // namespace dp::sema
