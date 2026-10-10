// t17: заготовка — напишите тела функций по task.md.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum class Align { Left, Right, Center, Justify };
enum class WrapError { None, BadWidth };

struct WrapResult {
    WrapError error;
    std::vector<std::string> lines;
};

WrapResult wrap(const std::string& text, int width, Align align) {
    // ...
}
