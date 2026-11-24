#version 450
// Decoded from: lib/moyou/lines1.fpo (SCE_FP_RSX)
// Primary PS3 XMB ribbon fragment shader.
// Note: RSX encoded scalar `val` via a 1D `_Encode` LUT for 8-bit 2-channel accumulation.
// In Vulkan with FP16 HDR render targets, `val` can be output directly (or encoded via uEncode).

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inParams;
layout(location = 2) in vec3 inEyeVec;
layout(location = 3) in vec3 inReflect;

layout(set = 0, binding = 1) uniform Lines1FragUBO {
    float uSpacing;
    float uThinness;
    float uUseEncodeLUT; // 0.0 = direct linear HDR, 1.0 = stock RSX _Encode LUT
    float uMipmapBias;   // LINE1.mnu MIPMAP BIAS
};

layout(set = 0, binding = 2) uniform sampler2D uStripes;
layout(set = 0, binding = 3) uniform sampler2D uFresLUT;
layout(set = 0, binding = 4) uniform sampler2D uEncode;

layout(location = 0) out vec4 outColor;

void main() {
    float u = fract(inUV.x * uSpacing);
    float stripeU = clamp(u - uThinness, 0.0, 1.0) / max(1.0 - uThinness, 1e-5);
    float stripe = texture(uStripes, vec2(stripeU, inUV.y), uMipmapBias).r;

    vec2 fres = texture(uFresLUT, inParams.zw).rg;
    float intensity = fres.g * inParams.y * stripe + fres.r * inParams.x;
    float val = intensity * inParams.w;

    if (uUseEncodeLUT > 0.5) {
        outColor = texture(uEncode, vec2(val, 0.5));
    } else {
        outColor = vec4(val, val, val, 1.0);
    }
}
