#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inShade;
layout(location = 3) in vec4 inW;
layout(location = 4) in vec2 inMaskUV;
layout(location = 5) in uint inCz;
layout(location = 6) in uint inTypes;
layout(location = 7) in uint inMeta;
layout(location = 0) out vec2 vUV;
layout(location = 1) out float vShade;
layout(push_constant) uniform PC {
    vec2 screen;
    vec2 _pad;
    float frac;
    float frame_i;
    float frames;
    float inv_frames;
} pc;
void main() {
    vec2 ndc = vec2(inPos.x / pc.screen.x * 2.0 - 1.0,
                    inPos.y / pc.screen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = inUV;
    vShade = inShade;
}
