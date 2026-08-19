#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inShadow;
layout(location = 3) in float inSortY;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vShadow;
layout(push_constant) uniform PC {
    vec2 screen;
} pc;
void main() {
    vec2 ndc = vec2(inPos.x / pc.screen.x * 2.0 - 1.0,
                    inPos.y / pc.screen.y * 2.0 - 1.0);
    /* Iso occlusion: larger sort_y (south) → larger Z; depth test GREATER. */
    float z = clamp(inSortY / max(pc.screen.y, 1.0), 0.001, 0.999);
    gl_Position = vec4(ndc, z, 1.0);
    vUV = inUV;
    vShadow = inShadow;
}
