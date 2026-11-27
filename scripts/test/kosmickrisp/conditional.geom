#version 450
layout(points, invocations = 2) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 1) flat in ivec2 ids[];
layout(location = 0) out vec4 color;

void main() {
    int instance = ids[0].y;
    int seed = gl_PrimitiveIDIn + gl_InvocationID + instance;
    if (seed % 3 == 0)
        return;

    ivec2 cell = ivec2(ids[0].x - 4, 2 * instance + gl_InvocationID);
    vec2 origin = vec2(3.25, 4.25) + vec2(cell * ivec2(20, 14));
    const vec2 corners[] = vec2[](vec2(0), vec2(6, 0), vec2(0, 6));
    for (int strip = 0; strip <= (seed & 1); ++strip) {
        for (int i = 0; i < 3; ++i) {
            vec2 p = origin + vec2(8 * strip, 0) + corners[i];
            gl_Position = vec4(p / 32.0 - 1.0, 0.5, 1.0);
            color = strip == 0 ? vec4(1, 0, 0, 1) : vec4(0, 1, 0, 1);
            EmitVertex();
        }
        EndPrimitive();
    }
}
