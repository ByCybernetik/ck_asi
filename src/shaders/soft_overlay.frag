#version 450
layout(set = 0, binding = 0) uniform sampler2D uSoft;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 c = texture(uSoft, vUV);
    /* Soft land off → black playfield; keep only lit sprite pixels (no CPU key). */
    float m = max(c.r, max(c.g, c.b));
    if (m < 0.045)
        discard;
    outColor = vec4(c.rgb, 1.0);
}
