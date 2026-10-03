// Эталон t03 на C++20. Над чертой — решение, под чертой — драйвер.
#include <cstdio>
#include <string>

struct State {
    enum Kind { Idle, Patrol, Chase, Flee } kind = Idle;
    int value = 0;  // waypoint, target или ticks
};

struct Event {
    enum Kind { Tick, SeeEnemy, LoseEnemy, LowHealth } kind;
    int id = 0;
};

State step(State s, Event e) {
    if (e.kind == Event::LowHealth) return {State::Flee, 3};
    if (e.kind == Event::SeeEnemy && s.kind != State::Flee) return {State::Chase, e.id};
    switch (s.kind) {
        case State::Chase:
            if (e.kind == Event::LoseEnemy) return {State::Patrol, 0};
            break;
        case State::Idle:
            if (e.kind == Event::Tick) return {State::Patrol, 0};
            break;
        case State::Patrol:
            if (e.kind == Event::Tick) return {State::Patrol, (s.value + 1) % 4};
            break;
        case State::Flee:
            if (e.kind == Event::Tick) return s.value > 1 ? State{State::Flee, s.value - 1} : State{State::Idle, 0};
            break;
    }
    return s;
}

std::string describe(State s) {
    switch (s.kind) {
        case State::Idle: return "idle";
        case State::Patrol: return "patrol " + std::to_string(s.value);
        case State::Chase: return "chase " + std::to_string(s.value);
        case State::Flee: return "flee " + std::to_string(s.value);
    }
    return "";
}

// --- driver ---
int main() {
    Event ev[] = {
        {Event::Tick}, {Event::Tick}, {Event::Tick}, {Event::Tick}, {Event::Tick},
        {Event::SeeEnemy, 7}, {Event::Tick}, {Event::SeeEnemy, 9}, {Event::LoseEnemy},
        {Event::LoseEnemy}, {Event::LowHealth}, {Event::SeeEnemy, 3}, {Event::Tick},
        {Event::Tick}, {Event::Tick}, {Event::Tick}, {Event::LowHealth}, {Event::LowHealth},
        {Event::Tick},
    };
    State s;
    std::printf("%s\n", describe(s).c_str());
    for (const Event& e : ev) {
        s = step(s, e);
        std::printf("%s\n", describe(s).c_str());
    }
    return 0;
}
