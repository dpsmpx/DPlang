// t03: заготовка — напишите тела функций по task.md.
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

static State make_state(State::Kind kind, int value) {
    State r;
    r.kind = kind;
    r.value = value;
    return r;
}

State step(State s, Event e) {
    // 1. LowHealth в любом состоянии.
    if (e.kind == Event::LowHealth) {
        return make_state(State::Flee, 3);
    }
    // 2. SeeEnemy в любом состоянии, кроме Flee.
    if (e.kind == Event::SeeEnemy && s.kind != State::Flee) {
        return make_state(State::Chase, e.id);
    }
    // 3. Chase + LoseEnemy.
    if (s.kind == State::Chase && e.kind == Event::LoseEnemy) {
        return make_state(State::Patrol, 0);
    }
    if (e.kind == Event::Tick) {
        switch (s.kind) {
            case State::Idle:  // 4.
                return make_state(State::Patrol, 0);
            case State::Patrol:  // 5.
                return make_state(State::Patrol, (s.value + 1) % 4);
            case State::Flee:  // 6.
                if (s.value > 1) {
                    return make_state(State::Flee, s.value - 1);
                }
                return make_state(State::Idle, 0);
            case State::Chase:
                break;
        }
    }
    // 7. Иначе состояние не меняется.
    return s;
}

std::string describe(State s) {
    switch (s.kind) {
        case State::Idle:
            return "idle";
        case State::Patrol:
            return "patrol " + std::to_string(s.value);
        case State::Chase:
            return "chase " + std::to_string(s.value);
        case State::Flee:
            return "flee " + std::to_string(s.value);
    }
    return "idle";
}
