#version 450
layout(constant_id = 0) const bool customIDs = false;
layout(location = 0) out vec4 pixel;
void main() {
    if (gl_PrimitiveID == (customIDs ? 37 : 0))
        pixel = vec4(1, 0, 0, 1);
    else if (gl_PrimitiveID == (customIDs ? 103 : 1))
        pixel = vec4(0, 1, 0, 1);
    else
        pixel = vec4(0, 0, 1, 1);
}
