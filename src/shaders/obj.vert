#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inLayer;
layout(location = 3) in float inShadow;
layout(location = 4) in float inPc;
layout(location = 5) in vec3 inTeam;
layout(location = 6) in float inSortY;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vLayer;
layout(location = 2) out float vShadow;
layout(location = 3) out float vPc;
layout(location = 4) out vec3 vTeam;
layout(push_constant) uniform PC {
    vec2 screen;
} pc;
void main() {
    vec2 ndc = vec2(inPos.x / pc.screen.x * 2.0 - 1.0,
                    inPos.y / pc.screen.y * 2.0 - 1.0);
    float z = clamp(inSortY / max(pc.screen.y, 1.0), 0.001, 0.999);
    gl_Position = vec4(ndc, z, 1.0);
    vUV = inUV;
    vLayer = inLayer;
    vShadow = inShadow;
    vPc = inPc;
    vTeam = inTeam;
}
