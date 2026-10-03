// Эталон t09 на C++20. Над чертой — решение, под чертой — драйвер.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

struct Slot { int item = 0; int count = 0; };  // item == 0 — пусто
struct Inventory { std::vector<Slot> slots; };

enum class InvError { None, Invalid, NotEnough };
struct AddResult { InvError error; int leftover; };

static int max_stack(int item) { return item < 100 ? 64 : 1; }

Inventory inv_new(int slots) { Inventory inv; inv.slots.resize(slots); return inv; }

AddResult inv_add(Inventory& inv, int item, int count) {
    if (item < 1 || count <= 0) return {InvError::Invalid, 0};
    int cap = max_stack(item);
    for (Slot& s : inv.slots) {
        if (count == 0) break;
        if (s.item == item && s.count < cap) {
            int put = std::min(cap - s.count, count);
            s.count += put;
            count -= put;
        }
    }
    for (Slot& s : inv.slots) {
        if (count == 0) break;
        if (s.item == 0) {
            int put = std::min(cap, count);
            s.item = item;
            s.count = put;
            count -= put;
        }
    }
    return {InvError::None, count};
}

int inv_total(const Inventory& inv, int item) {
    int t = 0;
    for (const Slot& s : inv.slots) if (s.item == item) t += s.count;
    return t;
}

InvError inv_remove(Inventory& inv, int item, int count) {
    if (item < 1 || count <= 0) return InvError::Invalid;
    if (inv_total(inv, item) < count) return InvError::NotEnough;
    for (int i = (int)inv.slots.size() - 1; i >= 0 && count > 0; i--) {
        Slot& s = inv.slots[i];
        if (s.item != item) continue;
        int take = std::min(s.count, count);
        s.count -= take;
        count -= take;
        if (s.count == 0) s.item = 0;
    }
    return InvError::None;
}

std::string inv_describe(const Inventory& inv) {
    std::string out;
    for (size_t i = 0; i < inv.slots.size(); i++) {
        if (i) out += ' ';
        const Slot& s = inv.slots[i];
        out += s.item == 0 ? std::string("-") : std::to_string(s.item) + ":" + std::to_string(s.count);
    }
    return out;
}

// --- driver ---
static void add(Inventory& inv, int item, int count) {
    AddResult r = inv_add(inv, item, count);
    if (r.error == InvError::Invalid) std::printf("add %d %d invalid\n", item, count);
    else std::printf("add %d %d left %d | %s\n", item, count, r.leftover, inv_describe(inv).c_str());
}
static void rem(Inventory& inv, int item, int count) {
    InvError e = inv_remove(inv, item, count);
    const char* t = e == InvError::None ? "ok" : e == InvError::NotEnough ? "not_enough" : "invalid";
    std::printf("remove %d %d %s | %s\n", item, count, t, inv_describe(inv).c_str());
}

int main() {
    Inventory inv = inv_new(4);
    add(inv, 1, 50);
    add(inv, 1, 30);
    add(inv, 200, 2);
    add(inv, 2, 100);
    add(inv, 0, 1);
    add(inv, 3, 0);
    rem(inv, 1, 70);
    rem(inv, 1, 20);
    rem(inv, 200, 1);
    add(inv, 2, 70);
    std::printf("total 2 = %d, total 1 = %d\n", inv_total(inv, 2), inv_total(inv, 1));
    return 0;
}
