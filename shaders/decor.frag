#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in float vShadow;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D atlas;
void main() {
    vec4 c = texture(atlas, vUV);
    if (c.a < 0.02)
        discard;
    if (vShadow > 0.5) {
        /* Retail drawmode=shadow: dark translucent blob under the sprite. */
        float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));
        float a = c.a * mix(0.55, 0.28, clamp(lum, 0.0, 1.0));
        outColor = vec4(0.0, 0.0, 0.0, a);
    } else {
        outColor = c;
    }
}
