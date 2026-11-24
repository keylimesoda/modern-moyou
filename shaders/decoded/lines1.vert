#version 450
// Decoded from: lib/moyou/lines1.vpo (SCE_VP_RSX)
// Primary PS3 XMB ribbon vertex shader.
// Note: Stock RSX shader passes inPos straight to gl_Position (SPU pre-transformed);
// if using unprojected vertices in Vulkan, multiply gl_Position by uMVM.

layout(location = 0) in vec4 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inNormal;

layout(set = 0, binding = 0) uniform Lines1VertexUBO {
    float uFresnel;
    float uBrightness;
    float uMipmapBias;
    float _pad0;
};

layout(location = 0) out vec2 outUV;
layout(location = 1) out vec4 outParams;
layout(location = 2) out vec3 outEyeVec;
layout(location = 3) out vec3 outReflect;

void main() {
    // inPos & inNormal arrive in exact OpenGL clip space (matching PS3 spline.elf + glFrustum)
    // so V, rawNdotV, and w match Sony RSX 1:1; remap only gl_Position to Vulkan NDC (Y-down, Z in [0, w]).
    gl_Position = vec4(inPos.x, -inPos.y, 0.5 * (inPos.z + inPos.w), inPos.w);

    vec3 N = normalize(inNormal.xyz);
    float invLenPos = inversesqrt(dot(inPos.xyz, inPos.xyz));
    vec3 V = inPos.xyz * invLenPos;

    float NdotV = dot(V, N);
    float rawNdotV = dot(V, inNormal.xyz);
    float w = invLenPos * abs(rawNdotV) * uMipmapBias;

    outEyeVec  = V;
    outReflect = V - 2.0 * NdotV * N; // reflect(V, N)
    outParams  = vec4(vec3(uFresnel, uBrightness, abs(NdotV)) * w, inUV.y);
    outUV      = vec2(inUV.x, w);
}
