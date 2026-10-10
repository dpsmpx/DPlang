// Драйвер образца s1 для переведённых вариантов dpl.
#include "cm_out.hpp"

int main() {
    std::vector<Shape> shapes = {Shape::Circle(1.0), Shape::Rect(2.0, 3.5), Shape::Square(2.5)};
    for (const Shape& s : shapes) std::printf("%s %.3f\n", name(s).c_str(), area(s));
    std::optional<Shape> big = largest(shapes);
    if (big) std::printf("largest %s\n", name(*big).c_str());
    if (!largest(std::vector<Shape>{})) std::printf("largest of none: none\n");
    Shape c = scale(shapes[0], 3.0);
    std::printf("scaled %s %.3f\n", name(c).c_str(), area(c));
    return 0;
}
