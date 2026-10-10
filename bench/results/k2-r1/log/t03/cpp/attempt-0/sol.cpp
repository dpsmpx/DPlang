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

State step(State s, Event e) {
    // ...
}

std::string describe(State s) {
    // ...
}
