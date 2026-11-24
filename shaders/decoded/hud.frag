#version 450
// Debug HUD bitmap font + translucent backdrop fragment shader.

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

layout(set = 0, binding = 0) uniform sampler2D uFontTex;

layout(location = 0) out vec4 outColor;

void main() {
    float cov = (inUV.x < 0.0) ? 1.0 : texture(uFontTex, inUV).r;
    outColor = vec4(inColor.rgb, inColor.a * cov);
}
