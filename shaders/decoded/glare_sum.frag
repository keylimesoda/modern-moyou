#version 450
// Decoded from: custom_render_plugin.prx HDR::make_glare mip accumulation (0x07b0a4..0x07c680)
// Combines all 5 blurred glare pyramid levels using exact Sony pow(GLARE_SUM_POW, k) / sum * GLARE_LEVEL weights.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform GlareSumUBO {
    vec4 uMipWeights0; // mips 0..3
    vec4 uMipWeights1; // x = mip 4
};

layout(set = 0, binding = 1) uniform sampler2D uMip0;
layout(set = 0, binding = 2) uniform sampler2D uMip1;
layout(set = 0, binding = 3) uniform sampler2D uMip2;
layout(set = 0, binding = 4) uniform sampler2D uMip3;
layout(set = 0, binding = 5) uniform sampler2D uMip4;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 sum = texture(uMip0, inUV).rgb * uMipWeights0.x
             + texture(uMip1, inUV).rgb * uMipWeights0.y
             + texture(uMip2, inUV).rgb * uMipWeights0.z
             + texture(uMip3, inUV).rgb * uMipWeights0.w
             + texture(uMip4, inUV).rgb * uMipWeights1.x;
    outColor = vec4(sum, 1.0);
}
