// Модуль money (после рефакторинга): суммы в копейках.
#pragma once
#include <cstdint>
#include <string>

enum class MoneyError { None, BadMoney };
struct MoneyResult { MoneyError error; int64_t cents; };

// "12.05", "-0.50", "3.00".
std::string fmt_money(int64_t cents);
// Разбор "D.DD": от 1 до 12 цифр, точка, ровно две цифры; без знака.
MoneyResult parse_money(const std::string& s);
