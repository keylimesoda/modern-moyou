#version 450
// Decoded from: lib/glutils/Gaussian.vpo (SCE_VP_RSX)
// 15-tap separable Gaussian vertex shader (precomputes symmetric +-k*offset UVs).

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;

layout(set = 0, binding = 0) uniform GaussianVertUBO {
    mat4 uModelviewProjection;
    vec2 uOffset;
    vec2 _pad0;
};

layout(location = 0) out vec2 outUV0;
layout(location = 1) out vec4 outUV1;
layout(location = 2) out vec4 outUV2;
layout(location = 3) out vec4 outUV3;
layout(location = 4) out vec4 outUV4;
layout(location = 5) out vec4 outUV5;
layout(location = 6) out vec4 outUV6;
layout(location = 7) out vec4 outUV7;

void main() {
    gl_Position = uModelviewProjection * vec4(inPos.xyz, 1.0);
    vec4 stepVec = vec4(uOffset, -uOffset);
    vec4 baseUV  = inUV.xyxy;

    outUV0 = inUV;
    outUV1 = baseUV + stepVec * 1.0;
    outUV2 = baseUV + stepVec * 2.0;
    outUV3 = baseUV + stepVec * 3.0;
    outUV4 = baseUV + stepVec * 4.0;
    outUV5 = baseUV + stepVec * 5.0;
    outUV6 = baseUV + stepVec * 6.0;
    outUV7 = baseUV + stepVec * 7.0;
}
