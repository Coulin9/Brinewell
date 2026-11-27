#version 450
layout(location = 0) flat out uint tag;
layout(constant_id = 0) const bool clipFirst = false;

void main() {
    // Front-facing left triangle, back-facing right triangle (frontFace=CLOCKWISE).
    const vec2 vertices[] = vec2[](vec2(6.25, 10.25), vec2(26.25, 10.25), vec2(6.25, 30.25),
                                  vec2(36.25, 10.25), vec2(36.25, 30.25), vec2(56.25, 10.25));
    int index = gl_VertexIndex;
    vec2 p = vertices[index];
    gl_Position = vec4(p / 32.0 - 1.0, clipFirst && index % 3 == 0 ? -0.5 : 0.5, 1.0);
    gl_PointSize = 1.0;
    // Only the original provoking vertex carries the expected value.
    tag = index % 3 == 0 ? 1u + uint(index / 3) : 19u;
}
