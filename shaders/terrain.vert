#version 450
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in float inShade;
layout(location = 3) in vec4 inW;
layout(location = 4) in vec2 inMaskUV; /* 0..1 in foreshortened 64×46 cell */
layout(location = 5) in uint inCz;     /* z00|z10<<8|z11<<16|z01<<24 */
layout(location = 6) in uint inTypes;  /* types[0..3] bytes */
layout(location = 7) in uint inMeta;   /* bit0=mask; bits8-9=nTypes; bits16-23=set_pack */

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vShade;
layout(location = 2) out vec4 vW;
layout(location = 3) out vec2 vMaskUV;
layout(location = 4) flat out uint vCz;
layout(location = 5) flat out uint vTypes;
layout(location = 6) flat out uint vMeta;

layout(push_constant) uniform PC {
    vec2 screen;
} pc;

void main() {
    vec2 ndc = vec2(inPos.x / pc.screen.x * 2.0 - 1.0,
                    inPos.y / pc.screen.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = inUV;
    vShade = inShade;
    vW = inW;
    vMaskUV = inMaskUV;
    vCz = inCz;
    vTypes = inTypes;
    vMeta = inMeta;
}
