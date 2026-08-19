#version 450
/* Dual-frame water: sample tall atlas at frame_i and frame_i+1, mix by frac.
 * vUV.x = u in [0,1] across texture width
 * vUV.y = v within one frame in [0,1]
 */
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vShade;
layout(location = 0) out vec4 outColor;
layout(push_constant) uniform PC {
    vec2 screen;
    vec2 _pad;
    float frac;       /* 0..1 between animate ticks */
    float frame_i;    /* current integer frame */
    float frames;     /* total frames in strip */
    float inv_frames; /* 1/frames == frame_h / tex_h */
} pc;
void main() {
    float fi = floor(pc.frame_i);
    float n = max(pc.frames, 1.0);
    float fj = mod(fi + 1.0, n);
    float vn = clamp(vUV.y, 0.0, 1.0) * pc.inv_frames;
    vec2 uv0 = vec2(vUV.x, vn + fi * pc.inv_frames);
    vec2 uv1 = vec2(vUV.x, vn + fj * pc.inv_frames);
    vec4 a = texture(uTex, uv0);
    vec4 b = texture(uTex, uv1);
    vec4 c = mix(a, b, clamp(pc.frac, 0.0, 1.0));
    float s = clamp(vShade, 0.0, 2.0);
    outColor = vec4(c.rgb * s, c.a);
}
