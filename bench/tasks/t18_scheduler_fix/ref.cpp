// t18: эталон на C++20.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct Event {
    int64_t at;
    int32_t priority;
    int32_t id;
    int64_t every;
    std::string name;
};

struct Scheduler {
    int64_t now;
    int32_t next_id;
    int32_t pending;
    std::vector<Event> heap;         // двоичная куча: вершина — ближайшее
    std::vector<int32_t> cancelled;  // отменённые номера; их записи в куче пропускаются
};

enum class SchedError { None, BadTime, NotFound };

struct AddResult {
    SchedError error;
    int32_t id;
};

Scheduler sched_new() { return Scheduler{0, 1, 0, {}, {}}; }

static bool earlier(const Event& a, const Event& b) {
    if (a.at != b.at) return a.at < b.at;
    if (a.priority != b.priority) return a.priority > b.priority;
    return a.id < b.id;
}

static void sift_up(std::vector<Event>& h, size_t i) {
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!earlier(h[i], h[p])) break;
        std::swap(h[i], h[p]);
        i = p;
    }
}

static void sift_down(std::vector<Event>& h, size_t i) {
    size_t n = h.size();
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && earlier(h[l], h[m])) m = l;
        if (r < n && earlier(h[r], h[m])) m = r;
        if (m == i) break;
        std::swap(h[i], h[m]);
        i = m;
    }
}

static void push(Scheduler& s, const Event& e) {
    s.heap.push_back(e);
    sift_up(s.heap, s.heap.size() - 1);
}

static Event pop(Scheduler& s) {
    Event top = s.heap[0];
    s.heap[0] = s.heap.back();
    s.heap.pop_back();
    if (!s.heap.empty()) sift_down(s.heap, 0);
    return top;
}

static bool is_cancelled(const Scheduler& s, int32_t id) {
    for (int32_t c : s.cancelled)
        if (c == id) return true;
    return false;
}

AddResult sched_add(Scheduler& s, int64_t at, int32_t priority, int64_t every, const std::string& name) {
    if (at < s.now || every < 0) return {SchedError::BadTime, 0};
    int32_t id = s.next_id++;
    push(s, Event{at, priority, id, every, name});
    s.pending++;
    return {SchedError::None, id};
}

SchedError sched_cancel(Scheduler& s, int32_t id) {
    bool queued = false;
    for (const Event& e : s.heap)
        if (e.id == id) queued = true;
    if (!queued || is_cancelled(s, id)) return SchedError::NotFound;
    s.cancelled.push_back(id);
    s.pending--;
    return SchedError::None;
}

std::vector<std::string> sched_run(Scheduler& s, int64_t until) {
    std::vector<std::string> out;
    if (until < s.now) return out;
    while (!s.heap.empty() && s.heap[0].at <= until) {
        Event e = pop(s);
        if (is_cancelled(s, e.id)) continue;
        out.push_back(std::to_string(e.at) + " " + e.name);
        if (e.every > 0) {
            e.at += e.every;
            push(s, e);
        } else {
            s.pending--;
        }
    }
    s.now = until;
    return out;
}

int32_t sched_pending(const Scheduler& s) { return s.pending; }

// --- driver ---
static void add(Scheduler& s, int64_t at, int32_t priority, int64_t every, const std::string& name) {
    AddResult r = sched_add(s, at, priority, every, name);
    if (r.error == SchedError::BadTime) std::printf("add %s: bad time\n", name.c_str());
    else std::printf("add %s: id %d\n", name.c_str(), r.id);
}

static void cancel(Scheduler& s, int32_t id) {
    SchedError e = sched_cancel(s, id);
    std::printf("cancel %d: %s, pending %d\n", id, e == SchedError::NotFound ? "not found" : "ok", sched_pending(s));
}

static void run(Scheduler& s, int64_t until) {
    std::vector<std::string> fired = sched_run(s, until);
    std::printf("run %lld:", (long long)until);
    for (const std::string& f : fired) std::printf(" [%s]", f.c_str());
    std::printf(" | pending %d\n", sched_pending(s));
}

int main() {
    Scheduler s = sched_new();
    add(s, 0, 0, 0, "boot");
    add(s, 10, 1, 0, "a");
    add(s, 10, 1, 0, "b");
    add(s, 10, 5, 0, "urgent");
    add(s, 10, 1, 0, "c");
    add(s, 5, 0, 7, "tick");
    add(s, 3, 0, -1, "neg");
    run(s, 10);
    add(s, 9, 0, 0, "late");
    add(s, 10, 0, 0, "now");
    run(s, 11);
    run(s, 20);
    cancel(s, 6);
    cancel(s, 6);
    cancel(s, 2);
    cancel(s, 99);
    run(s, 40);
    run(s, 30);
    // Много событий — порядок кучи.
    Scheduler h = sched_new();
    uint64_t x = 12345;
    for (int i = 0; i < 40; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        int64_t at = (int64_t)((x >> 33) % 16);
        int32_t pr = (int32_t)((x >> 20) % 4);
        AddResult r = sched_add(h, at, pr, 0, "e" + std::to_string(i));
        if (r.error != SchedError::None) std::printf("unexpected error\n");
    }
    cancel(h, 7);
    cancel(h, 8);
    cancel(h, 7);
    run(h, 7);
    run(h, 15);
    // Периодические с разными периодами и отмена в середине.
    Scheduler p = sched_new();
    add(p, 1, 0, 3, "p3");
    add(p, 2, 1, 4, "p4");
    add(p, 4, 2, 0, "once");
    run(p, 9);
    run(p, 9);
    cancel(p, 1);
    run(p, 18);
    add(p, 17, 0, 1, "past");
    add(p, 18, 9, 2, "fast");
    run(p, 22);
    cancel(p, 3);
    return 0;
}
