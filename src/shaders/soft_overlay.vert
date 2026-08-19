#version 450
/* Fullscreen triangle: soft retail sprites over GPU terrain. */
layout(location = 0) out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
