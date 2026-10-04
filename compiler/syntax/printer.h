// Печать модуля в каноническом виде (основа `dplang fmt`, шаг 10):
// один формат без настроек (Р7). Литералы печатаются так, как записаны
// в исходнике. Комментарии печать пока не переносит — это делает fmt.
#pragma once

#include <string>
#include <string_view>

#include "syntax/ast.h"

namespace dp {

std::string print_module(const ast::Module& m, std::string_view source);

}  // namespace dp
