/* Хост включает заголовок C со структурой и заголовок библиотеки. */
#include <stdio.h>

#include "point.h"
#include "cheader.h"

int main(void) {
    Point p = {3, 4};
    struct Point q = point_swap(p);
    printf("%d %d %d\n", point_sum(p), q.x, q.y);
    return 0;
}
