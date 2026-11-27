#version 450
layout(location = 0) out vec4 color;
void main() {
    const vec2 triangle[] = vec2[](vec2(6.25, 10.25), vec2(26.25, 10.25), vec2(6.25, 30.25));
    bool right = gl_VertexIndex >= 3;
    int i = gl_VertexIndex % 3;
    vec2 p = triangle[i] + vec2(right ? 30.0 : 0.0, 0.0);
    // Left depth slope is 0.5 / 20 per pixel. Right is the flat-depth control.
    gl_Position = vec4(p / 32.0 - 1.0, !right && i == 1 ? 0.7 : 0.2, 1.0);
    gl_PointSize = 1.0;
    color = vec4(1, 0, 0, 1);
}
