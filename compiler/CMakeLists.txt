# Компилятор dpl (Р34: C++20 в подмножестве Galdhold — без исключений
# и RTTI, без внешних зависимостей). Сборка и тесты — docs/compiler/README.md.
cmake_minimum_required(VERSION 3.20)
project(dpl CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DPL_SANITIZE "Собрать компилятор с ASan и UBSan" OFF)

add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
if(DPL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(dpcore STATIC
    api/compile.cpp
    api/contract.cpp
    backend_c/emit_c.cpp
    ir/dump.cpp
    ir/interp.cpp
    ir/ir.cpp
    ir/lower.cpp
    ir/verify.cpp
    support/source.cpp
    support/diag.cpp
    support/json_read.cpp
    sema/access.cpp
    sema/check.cpp
    sema/check_expr.cpp
    sema/check_pat.cpp
    sema/errinfer.cpp
    sema/linear.cpp
    sema/parallel.cpp
    sema/collect.cpp
    sema/loader.cpp
    sema/resolve.cpp
    sema/summary.cpp
    sema/types.cpp
    syntax/ast_dump.cpp
    syntax/depth.cpp
    syntax/lexer.cpp
    syntax/literal.cpp
    syntax/parser.cpp
    syntax/printer.cpp
)
target_include_directories(dpcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)

add_executable(dpl driver/main.cpp driver/big_stack.cpp driver/build.cpp driver/sandbox.cpp driver/worker.cpp)
target_link_libraries(dpl PRIVATE dpcore Threads::Threads)

# Пробник песочницы worker-а: запирается и пробует запрещённое
# (tests/service, docs/service/README.md, «Изоляция»).
add_executable(dpsandbox_probe driver/sandbox_probe.cpp driver/sandbox.cpp driver/big_stack.cpp)
target_include_directories(dpsandbox_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dpsandbox_probe PRIVATE Threads::Threads)

# Фаззер «не падает»: tests/fuzz (docs/compiler/README.md).
add_executable(dpfuzz fuzz/main.cpp)
target_link_libraries(dpfuzz PRIVATE dpcore)
