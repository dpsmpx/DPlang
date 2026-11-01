/* Хост библиотеки main.dpm (проход lib): функции C, которые пишут в
 * цепочку вывода, как Vulkan в pNext (Р77). */
#include <stdint.h>
#include <stdio.h>

#include "chain.h"

struct OutBase {
    uint32_t kind;
    struct OutBase* next;
};

struct Props {
    uint32_t kind;
    struct OutBase* next;
    int32_t count;
};

struct Limits {
    uint32_t kind;
    struct OutBase* next;
    int32_t max_size;
    int32_t max_count;
};

struct InBase {
    uint32_t kind;
    const struct InBase* next;
};

struct Create {
    uint32_t kind;
    const struct InBase* next;
    uint32_t flags;
};

struct Pair {
    struct Props props;
    float weight;
};

void host_fill(struct Props* p) {
    printf("fill kind=%u next=%s\n", p->kind, p->next ? "set" : "null");
    p->count = 7;
    for (struct OutBase* b = p->next; b; b = b->next) {
        printf("  link kind=%u\n", b->kind);
        if (b->kind == 2) {
            struct Limits* l = (struct Limits*)b;
            l->max_size = 64;
            l->max_count = 3;
        }
    }
}

void host_fill_all(struct Props* ps, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        printf("all %d kind=%u next=%s\n", i, ps[i].kind, ps[i].next ? "set" : "null");
        ps[i].count = 10 * (i + 1);
    }
}

void host_pair(struct Pair* p) {
    printf("pair weight=%.2f\n", (double)p->weight);
    host_fill(&p->props);
}

int32_t host_create(const struct Create* c) {
    int32_t sum = 0;
    printf("create kind=%u flags=%u\n", c->kind, c->flags);
    for (const struct InBase* b = c->next; b; b = b->next) {
        printf("  in kind=%u\n", b->kind);
        if (b->kind == 1) sum += ((const struct Props*)b)->count;
        if (b->kind == 2) sum += ((const struct Limits*)b)->max_size * ((const struct Limits*)b)->max_count;
    }
    return sum;
}

int main(void) {
    printf("%d\n", chain_run());
    return 0;
}
