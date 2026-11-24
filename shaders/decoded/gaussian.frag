#version 450
// Decoded from: lib/glutils/Gaussian.fpo (SCE_FP_RSX)
// 15-tap separable Gaussian with independent RGB per-channel weights (chromatic bloom).

layout(location = 0) in vec2 inUV0;
layout(location = 1) in vec4 inUV1;
layout(location = 2) in vec4 inUV2;
layout(location = 3) in vec4 inUV3;
layout(location = 4) in vec4 inUV4;
layout(location = 5) in vec4 inUV5;
layout(location = 6) in vec4 inUV6;
layout(location = 7) in vec4 inUV7;

layout(set = 0, binding = 1) uniform GaussianFragUBO {
    vec4 uWeights[8]; // .rgb allows distinct Gaussian radii for R, G, and B
};

layout(set = 0, binding = 2) uniform sampler2D uTexSamp;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 sum = texture(uTexSamp, inUV0).rgb * uWeights[0].rgb;
    sum += (texture(uTexSamp, inUV1.xy).rgb + texture(uTexSamp, inUV1.zw).rgb) * uWeights[1].rgb;
    sum += (texture(uTexSamp, inUV2.xy).rgb + texture(uTexSamp, inUV2.zw).rgb) * uWeights[2].rgb;
    sum += (texture(uTexSamp, inUV3.xy).rgb + texture(uTexSamp, inUV3.zw).rgb) * uWeights[3].rgb;
    sum += (texture(uTexSamp, inUV4.xy).rgb + texture(uTexSamp, inUV4.zw).rgb) * uWeights[4].rgb;
    sum += (texture(uTexSamp, inUV5.xy).rgb + texture(uTexSamp, inUV5.zw).rgb) * uWeights[5].rgb;
    sum += (texture(uTexSamp, inUV6.xy).rgb + texture(uTexSamp, inUV6.zw).rgb) * uWeights[6].rgb;
    sum += (texture(uTexSamp, inUV7.xy).rgb + texture(uTexSamp, inUV7.zw).rgb) * uWeights[7].rgb;
    outColor = vec4(sum, 0.0);
}
