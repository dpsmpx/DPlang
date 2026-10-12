/* Хост библиотеки main.dpm (проход lib): объекты, первое слово которых —
 * адрес таблицы функций (как JNIEnv: (*env)->F(env, ...)). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "slots.h"

struct Obj;

struct Table {
    void* reserved;
    int32_t (*add)(struct Obj*, int32_t, int32_t);
    const char* (*name)(struct Obj*);
    void (*set)(struct Obj*, int32_t, int32_t*);
    struct Obj* (*find)(struct Obj*, const char*);
};

struct Obj {
    const struct Table* table;
    int32_t base;
};

static struct Obj objs[2];

static int32_t sum(struct Obj* o, int32_t a, int32_t b) { return a + b + o->base; }
static int32_t product(struct Obj* o, int32_t a, int32_t b) { return a * b + o->base; }
static const char* first_name(struct Obj* o) { (void)o; return "first"; }
static const char* second_name(struct Obj* o) { (void)o; return "second"; }

static void set(struct Obj* o, int32_t v, int32_t* out) {
    o->base = v;
    *out = v * 10;
}

static struct Obj* find(struct Obj* o, const char* name) {
    (void)o;
    if (!name) return NULL;
    return strcmp(name, "second") == 0 ? &objs[1] : &objs[0];
}

static const struct Table first = {NULL, sum, first_name, set, find};
static const struct Table second = {NULL, product, second_name, set, find};

void* host_obj(int32_t which) { return &objs[which]; }

int main(void) {
    objs[0].table = &first;
    objs[1].table = &second;
    printf("%d\n", slots_run());
    return 0;
}
