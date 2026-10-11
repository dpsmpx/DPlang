/* Хост на C библиотеки main.dpm (проход lib, tests/run.py): вызывает
 * функции @export и передаёт им колбэк; последняя — ловушка dpl. */
#include <stdio.h>

#include "game.h"

static int32_t twice(void* ctx, int32_t x) { return x * 2 + (ctx != NULL); }

int main(void) {
    printf("%d\n", game_add(2, 3));
    struct Vec2 v = {3.0f, 4.0f};
    printf("%g\n", (double)game_len2(v));
    fflush(stdout);
    printf("%d\n", game_greet("world"));
    game_scale(&v, 2.0f);
    printf("%g %g\n", (double)v.x, (double)v.y);
    int dummy = 0;
    printf("%d\n", game_apply(twice, &dummy, 20));
    printf("%d %d\n", game_has(&dummy), game_has(NULL));
    fflush(stdout);
    printf("%d\n", game_div(1, 0));
    return 0;
}
