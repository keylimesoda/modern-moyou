#version 450
// Decoded from: lib/moyou/background.fpo (SCE_FP_RSX)

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inGradCol;

layout(set = 0, binding = 1) uniform sampler2D uTex;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 texCol = texture(uTex, inUV).rgb;
    outColor = vec4(texCol * inGradCol.rgb, 1.0);
}
