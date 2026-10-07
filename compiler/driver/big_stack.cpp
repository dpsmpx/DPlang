// Поток с большим стеком (big_stack.h).
#include "driver/big_stack.h"

#include <pthread.h>

namespace dp {

namespace {

struct Call {
    void (*fn)(void*);
    void* arg;
};

void* trampoline(void* p) {
    auto* c = static_cast<Call*>(p);
    c->fn(c->arg);
    return nullptr;
}

}  // namespace

bool run_on_stack(size_t stack_bytes, void (*fn)(void*), void* arg) {
    Call c{fn, arg};
    pthread_attr_t attr;
    pthread_t th;
    bool attr_ok = pthread_attr_init(&attr) == 0;
    bool ok = attr_ok && pthread_attr_setstacksize(&attr, stack_bytes) == 0 &&
              pthread_create(&th, &attr, trampoline, &c) == 0;
    if (attr_ok) pthread_attr_destroy(&attr);
    if (ok) pthread_join(th, nullptr);
    return ok;
}

void run_with_stack(size_t stack_bytes, void (*fn)(void*), void* arg) {
    if (!run_on_stack(stack_bytes, fn, arg)) fn(arg);
}

}  // namespace dp
