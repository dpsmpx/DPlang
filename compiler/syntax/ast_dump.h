// Отладочный дамп AST (`dpl debug parse`): по строке на узел, отступ —
// вложенность. Формат не стабилен (S2); участков в нём нет, поэтому
// дампы исходника и его печати совпадают — на этом стоит проверка круга
// «разбор → печать → разбор».
#pragma once

#include <functional>
#include <string>

#include "syntax/ast.h"

namespace dp {

std::string dump_module(const ast::Module& m);
// Дамп с пометками выражений: строка, которую annotate добавит к узлу
// (`debug types` пишет так тип и неявное преобразование).
std::string dump_module(const ast::Module& m, const std::function<std::string(const ast::Expr*)>& annotate);
// Тип и образец — в одну строку, в записи языка.
std::string type_text(const ast::Type* t);
std::string pat_text(const ast::Pat* p);
std::string_view bin_op_text(ast::BinOp op);
std::string_view assign_op_text(ast::AssignOp op);

}  // namespace dp
