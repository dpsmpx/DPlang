// Модуль report (после рефакторинга): текстовое описание мира.
#pragma once
#include <cstdint>
#include <string>

#include "world.hpp"

// "0:22 1:dead 2:50" — номер и здоровье или dead для каждого юнита.
std::string describe(const World& w);
// Число живых юнитов.
int32_t alive_count(const World& w);
