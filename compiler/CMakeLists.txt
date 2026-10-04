# Компилятор DPlang (Р34: C++20 в подмножестве VoxelRPG — без исключений
# и RTTI, без внешних зависимостей). Сборка и тесты — docs/compiler/README.md.
cmake_minimum_required(VERSION 3.20)
project(dplang CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DPLANG_SANITIZE "Собрать компилятор с ASan и UBSan" OFF)

add_compile_options(-fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror)
if(DPLANG_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(dpcore STATIC
    support/source.cpp
    support/diag.cpp
    syntax/ast_dump.cpp
    syntax/lexer.cpp
    syntax/literal.cpp
    syntax/parser.cpp
    syntax/printer.cpp
)
target_include_directories(dpcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(dplang driver/main.cpp)
target_link_libraries(dplang PRIVATE dpcore)

# Фаззер «не падает»: tests/fuzz (docs/compiler/README.md).
add_executable(dpfuzz fuzz/main.cpp)
target_link_libraries(dpfuzz PRIVATE dpcore)
