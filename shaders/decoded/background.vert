#version 450
// Decoded from: lib/moyou/background.vpo (SCE_VP_RSX)

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;

layout(set = 0, binding = 0) uniform BackgroundUBO {
    mat4 uMVM;
    vec4 uColour1;
    vec4 uColour2;
};

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outGradCol;

void main() {
    gl_Position = uMVM * inPos;
    outUV       = inUV;
    outGradCol  = mix(uColour1, uColour2, inUV.y);
}
