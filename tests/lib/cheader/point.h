/* Заголовок C со структурой: хост включает и его, и заголовок
 * библиотеки dpl (Р76) — структура определена один раз. */
#ifndef POINT_H
#define POINT_H
#include <stdint.h>

typedef struct Point {
    int32_t x;
    int32_t y;
} Point;

#endif
