// Модуль money (до рефакторинга): суммы в копейках.
#pragma once
#include <cstdint>
#include <string>

// "12.05", "-0.50", "3.00".
std::string fmt_money(int64_t cents);
// Разбор "D.DD": от 1 до 12 цифр, точка, ровно две цифры; без знака.
// -1 — неверная запись.
int64_t parse_money(const std::string& s);
