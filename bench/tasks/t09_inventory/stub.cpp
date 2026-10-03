// t09: заготовка — напишите тела функций по task.md.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

struct Inventory {
    // поля — на ваш выбор
};

enum class InvError { None, Invalid, NotEnough };

struct AddResult { InvError error; int leftover; };

Inventory inv_new(int slots) {
    // ...
}

AddResult inv_add(Inventory& inv, int item, int count) {
    // ...
}

int inv_total(const Inventory& inv, int item) {
    // ...
}

InvError inv_remove(Inventory& inv, int item, int count) {
    // ...
}

std::string inv_describe(const Inventory& inv) {
    // ...
}
