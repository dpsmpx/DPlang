// Общий вход компиляции (compile.h).
#include "api/compile.h"

namespace dp::api {

std::unique_ptr<sema::Program> check(const std::string& root_path, const CompileOptions& opt, SourceMap& sources,
                                     Diagnostics& diags) {
    sema::LoadOptions lo;
    lo.stdlib_dir = opt.stdlib_dir;
    lo.bindings_dir = opt.bindings_dir;
    lo.reader = opt.reader;
    lo.max_nesting = opt.max_nesting;
    auto prog = sema::load_program(root_path, lo, sources, diags);
    if (!prog || !prog->load_ok) return prog;
    sema::collect(*prog, diags);
    sema::check(*prog, diags);
    return prog;
}

bool MemorySources::read(const std::string& path, std::string& out) const {
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    out = it->second;
    return true;
}

}  // namespace dp::api
