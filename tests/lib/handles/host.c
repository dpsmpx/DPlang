/* Хост библиотеки main.dpm (проход lib): дескрипторы и указатели на
 * функции C в раскладке C — указатель или NULL, как у Vulkan. */
#include <stdint.h>
#include <stdio.h>

#include "handles.h"

struct Dev {
    uint32_t id;
};

static struct Dev devs[3] = {{1}, {2}, {3}};

typedef int32_t (*event_fn)(int32_t);

struct Info {
    uint32_t id;
    event_fn on_event;
};

struct Props {
    void* device;
    uint32_t id;
    event_fn on_event;
    uint8_t tag[4];
};

int32_t host_create(const struct Info* info, void** out) {
    printf("host create id=%u event=%d\n", info->id, info->on_event ? info->on_event(21) : -1);
    *out = &devs[0];
    return 0;
}

int32_t host_enumerate(uint32_t* count, void** out) {
    if (!out) {
        *count = 3;
        return 0;
    }
    for (uint32_t i = 0; i < *count && i < 3; i++) out[i] = &devs[i];
    return 0;
}

void host_props(void* device, struct Props* out) {
    struct Dev* d = device;
    out->device = device;
    out->id = d->id;
    out->on_event = d->id == 2 ? twice : NULL;
    for (int i = 0; i < 4; i++) out->tag[i] = (uint8_t)(i < (int)d->id ? 'a' + i : 0);
}

struct Info host_info(void) {
    struct Info r = {5, NULL};
    return r;
}

bool host_same(void* a, void* b) { return a == b; }

int main(void) {
    printf("%d\n", handles_run());
    return 0;
}
