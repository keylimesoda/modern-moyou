#version 450
// Decoded from: lib/glutils/GaussBloomAndBlur.fpo (SCE_FP_RSX)
// Single-pass 15-tap MRT separable bloom threshold + blur.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform GaussBloomUBO {
    vec4 uWeights;     // taps 0..3
    vec4 uWeights2;    // taps 4..7
    vec2 uOffset;
    float uThreshold;
    float uGlareWeight;
};

layout(set = 0, binding = 1) uniform sampler2D uTexSamp;

layout(location = 0) out vec4 outBloomCol;
layout(location = 1) out vec4 outBlurCol;

vec4 bloomExtract(vec4 c) {
    return clamp(max(c - vec4(uThreshold), vec4(0.0)) * uGlareWeight, vec4(0.0), vec4(1.0));
}

void main() {
    float w[8] = float[8](
        uWeights.x,  uWeights.y,  uWeights.z,  uWeights.w,
        uWeights2.x, uWeights2.y, uWeights2.z, uWeights2.w
    );

    vec4 c0 = texture(uTexSamp, inUV);
    vec4 blurSum  = c0 * w[0];
    vec4 bloomSum = bloomExtract(c0) * w[0];

    for (int i = 1; i <= 7; ++i) {
        vec2 off = uOffset * float(i);
        vec4 cp = texture(uTexSamp, inUV + off);
        vec4 cm = texture(uTexSamp, inUV - off);
        blurSum  += (cp + cm) * w[i];
        bloomSum += (bloomExtract(cp) + bloomExtract(cm)) * w[i];
    }

    outBloomCol = bloomSum;
    outBlurCol  = blurSum;
}
