// Печать модуля в каноническом виде — `dpl fmt`: один формат без
// настроек (Р7). Литералы печатаются так, как записаны в исходнике;
// комментарии и пустые строки (не больше одной подряд) сохраняются.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "syntax/ast.h"

namespace dp {

std::string print_module(const ast::Module& m, std::string_view source);

// Тексты комментариев модуля по порядку, без пробелов в конце — для
// проверки, что печать их не теряет и не повторяет.
std::vector<std::string> comment_texts(const ast::Module& m, std::string_view source);

}  // namespace dp
