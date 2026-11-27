#version 450
layout(points) in;
layout(points, max_vertices = 1) out;
layout(set = 0, binding = 0, std430) buffer Result {
    uvec4 record[];
};

void main() {
    uint id = uint(gl_PrimitiveIDIn);
    uvec2 p = uvec2((gl_in[0].gl_Position.xy + 1.0) * 32.0);
    atomicAdd(record[id].x, 1u);
    record[id].yzw = uvec3(p, 0xabcd0123u ^ id);
    gl_Position = gl_in[0].gl_Position;
    EmitVertex();
    EndPrimitive();
}
