#version 450
layout(location = 0) flat in uint tag;
layout(location = 0) out vec4 pixel;
void main() {
    if (tag == 1u)
        pixel = gl_FrontFacing ? vec4(1, 0, 0, 1) : vec4(1, 1, 0, 1);
    else if (tag == 2u)
        pixel = gl_FrontFacing ? vec4(1, 0, 1, 1) : vec4(0, 1, 0, 1);
    else
        pixel = vec4(0, 0, 1, 1);
}
