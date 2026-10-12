/* Хост библиотеки main.dpm (проход lib): функции C, которые получают
 * заимствующие структуры dpl, и проверка их раскладки C. */
#include <stdint.h>
#include <stdio.h>

#include "borrow.h"

struct Base {
    uint32_t kind;
    const struct Base* next;
};

struct Extra {
    uint32_t kind;
    const struct Base* next;
    int32_t bonus;
};

struct Item {
    const char* name;
    float weight;
};

struct Order {
    uint32_t kind;
    const struct Base* next;
    const char* title;
    const struct Item* items;
    uint32_t item_count;
    const char** tags;
    uint32_t tag_count;
    const struct Item* first;
};

int32_t host_order(const struct Order* o) {
    printf("order kind=%u title=%s items=%u tags=%u\n", o->kind, o->title, o->item_count, o->tag_count);
    for (uint32_t i = 0; i < o->item_count; i++) printf("  item %s %.2f\n", o->items[i].name, (double)o->items[i].weight);
    for (uint32_t i = 0; i < o->tag_count; i++) printf("  tag %s\n", o->tags[i]);
    const struct Extra* e = (const struct Extra*)o->next;
    printf("  next kind=%u bonus=%d first=%s %.2f\n", e->kind, e->bonus, o->first->name, (double)o->first->weight);
    return (int32_t)(o->item_count + o->tag_count);
}

int32_t host_names(const char* const* names, int32_t n) {
    int32_t len = 0;
    for (int32_t i = 0; i < n; i++) {
        printf("  name %s\n", names[i]);
        for (const char* p = names[i]; *p; p++) len++;
    }
    return len;
}

int32_t host_opt(const struct Item* item) {
    if (!item) return 100;
    printf("  opt %s %.2f\n", item->name, (double)item->weight);
    return 1000;
}

int main(void) {
    printf("%d\n", borrow_run());
    return 0;
}
