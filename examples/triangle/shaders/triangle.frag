#version 450
// Сплошной цвет: 1.0, 0.6, 0.2 → 255, 153, 51 в UNORM без округлений на
// границе.
layout(location = 0) out vec4 color;

void main() {
    color = vec4(1.0, 0.6, 0.2, 1.0);
}
