// Драйвер t03 для переведённых вариантов C- (тот же вывод, что у ref.cpp).
#include "cm_out.hpp"

int main() {
    Event ev[] = {
        Event::Tick(), Event::Tick(), Event::Tick(), Event::Tick(), Event::Tick(),
        Event::SeeEnemy(7), Event::Tick(), Event::SeeEnemy(9), Event::LoseEnemy(),
        Event::LoseEnemy(), Event::LowHealth(), Event::SeeEnemy(3), Event::Tick(),
        Event::Tick(), Event::Tick(), Event::Tick(), Event::LowHealth(), Event::LowHealth(),
        Event::Tick(),
    };
    State s = State::Idle();
    std::printf("%s\n", describe(s).c_str());
    for (const Event& e : ev) {
        s = step(s, e);
        std::printf("%s\n", describe(s).c_str());
    }
    return 0;
}
