#version 450
// Simple 2D pixel-space debug HUD overlay shader (rendered after tonemapping).

layout(location = 0) in vec2 inPosPx;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform HudPush {
    vec2 uInvScreenSize;
};

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outColor;

void main() {
    vec2 ndc = inPosPx * uInvScreenSize * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    outUV = inUV;
    outColor = inColor;
}
