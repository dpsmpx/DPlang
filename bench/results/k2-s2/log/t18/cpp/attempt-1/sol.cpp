// t18: реализация с ошибками — исправить, не переписывая заново.
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
