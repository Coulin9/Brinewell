#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;
layout(location = 0) in vec4 vertexColor[];
layout(location = 0) out vec4 color;

void main() {
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 3; ++i) {
            vec2 p = 0.35 * gl_in[i].gl_Position.xy;
            p.x += side == 0 ? -0.48 : 0.48;
            gl_Position = vec4(p, 0.5, 1.0);
            color = side == 0 ? vertexColor[i] : vertexColor[i].grba;
            EmitVertex();
        }
        EndPrimitive();
    }
}
