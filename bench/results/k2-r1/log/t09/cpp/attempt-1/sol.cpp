// t09: заготовка — напишите тела функций по task.md.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

struct Slot {
    int item;   // 0 — пустая ячейка
    int count;
};

struct Inventory {
    std::vector<Slot> cells;
};

enum class InvError { None, Invalid, NotEnough };

struct AddResult { InvError error; int leftover; };

static int max_stack(int item) { return item < 100 ? 64 : 1; }

Inventory inv_new(int slots) {
    Inventory inv;
    if (slots < 0) slots = 0;
    inv.cells.assign(static_cast<size_t>(slots), Slot{0, 0});
    return inv;
}

AddResult inv_add(Inventory& inv, int item, int count) {
    if (count <= 0 || item < 1) return AddResult{InvError::Invalid, 0};
    const int cap = max_stack(item);
    int left = count;
    for (Slot& s : inv.cells) {
        if (left == 0) break;
        if (s.count > 0 && s.item == item && s.count < cap) {
            int take = std::min(left, cap - s.count);
            s.count += take;
            left -= take;
        }
    }
    for (Slot& s : inv.cells) {
        if (left == 0) break;
        if (s.count == 0) {
            int take = std::min(left, cap);
            s.item = item;
            s.count = take;
            left -= take;
        }
    }
    return AddResult{InvError::None, left};
}

int inv_total(const Inventory& inv, int item) {
    int sum = 0;
    for (const Slot& s : inv.cells)
        if (s.count > 0 && s.item == item) sum += s.count;
    return sum;
}

InvError inv_remove(Inventory& inv, int item, int count) {
    if (inv_total(inv, item) < count) return InvError::NotEnough;
    int left = count;
    for (size_t i = inv.cells.size(); i > 0 && left > 0; --i) {
        Slot& s = inv.cells[i - 1];
        if (s.count > 0 && s.item == item) {
            int take = std::min(left, s.count);
            s.count -= take;
            left -= take;
            if (s.count == 0) s.item = 0;
        }
    }
    return InvError::None;
}

std::string inv_describe(const Inventory& inv) {
    std::string out;
    for (size_t i = 0; i < inv.cells.size(); ++i) {
        if (i > 0) out += ' ';
        const Slot& s = inv.cells[i];
        if (s.count == 0) {
            out += '-';
        } else {
            out += std::to_string(s.item);
            out += ':';
            out += std::to_string(s.count);
        }
    }
    return out;
}
