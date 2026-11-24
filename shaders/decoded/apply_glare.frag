#version 450
// Decoded from: lib/glutils/ApplyGlare.fpo (SCE_FP_RSX)
// Linearly combines two glare/scene mip buffers.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform ApplyGlareUBO {
    vec4 uWeight1; // x = weight for tex0, y = weight for tex1
};

layout(set = 0, binding = 1) uniform sampler2D uTexSamp0;
layout(set = 0, binding = 2) uniform sampler2D uTexSamp1;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 c0 = texture(uTexSamp0, inUV).rgb;
    vec3 c1 = texture(uTexSamp1, inUV).rgb;
    outColor = vec4(c0 * uWeight1.x + c1 * uWeight1.y, 1.0);
}
