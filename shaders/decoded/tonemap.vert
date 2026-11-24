#version 450
// Fullscreen quad vertex shader for post-processing passes (ToneMap, ApplyGlare, GaussBloomAndBlur).

layout(location = 0) out vec2 outUV;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    outUV = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
