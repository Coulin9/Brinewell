#version 450
layout(location = 0) out vec4 color;
layout(location = 1) flat out ivec2 ids;

void main() {
    const vec2 vertices[3] = vec2[](vec2(8.25, 8.25), vec2(56.25, 8.25),
                                    vec2(8.25, 56.25));
    int id = gl_VertexIndex;
    // The indexed-indirect case fetches 6, 4, 5 after the base vertex offset.
    bool indexed = id >= 4 && id < 7;
    vec2 p = vertices[indexed ? id - 4 : clamp(id, 0, 2)];
    if (indexed)
        p += vec2(3.0, 5.0);
    if (id >= 7)
        p = vec2(2.25 + id % 42, 3.25 + id / 42);
    gl_Position = vec4(p / 32.0 - 1.0, 0.5, 1.0);
    gl_PointSize = 1.0;
    color = vec4(1, 0, 0, 1);
    ids = ivec2(id, gl_InstanceIndex);
}
