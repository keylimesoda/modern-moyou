#version 450
// Decoded from: lib/particles/particles_second.fpo (SCE_FP_RSX)
// Specular iridescent starburst glare pass for XMB bokeh sparkles.

layout(location = 0) in vec4 inUV0;
layout(location = 1) in vec4 inUV2;
layout(location = 2) in vec4 inUV3;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inWorldPos;
layout(location = 5) in vec4 inNormalV;

layout(set = 0, binding = 1) uniform ParticlesFragUBO {
    mat4 uLightPack;
    vec4 uColor;
    vec4 uNearControl;
    float uIridescentExp;
    float uGamma;
    vec2 _pad0;
    vec4 uGlare; // x = glare, y = glare_scale, z = glare_p1, w = glare_p2
};

layout(set = 0, binding = 2) uniform sampler2D uIridescentTex;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 iridTex = texture(uIridescentTex, inNormalV.xy * 0.5 + 0.5).rgb;
    float iridStr = uLightPack[3].w;
    vec3 irid = pow(max(vec3(1.0) + (iridTex - vec3(1.0)) * iridStr, vec3(1e-5)), vec3(uIridescentExp));

    vec3 eyePos   = vec3(uLightPack[0].x, uLightPack[1].x, uLightPack[2].x);
    vec3 lightPos = vec3(uLightPack[0].y, uLightPack[1].y, uLightPack[2].y);

    vec3 V = normalize(eyePos - inWorldPos.xyz);
    vec3 toLight = lightPos - inWorldPos.xyz;
    float distL = length(toLight);
    vec3 L = toLight / max(distL, 1e-6);
    vec3 H = normalize(L + V);

    float spec = pow(max(abs(dot(inNormal, H)), 1e-6), uLightPack[3].y) * uLightPack[1].w;
    float atten = uLightPack[0].z + distL * (uLightPack[1].z + distL * uLightPack[2].z);
    vec3 lit = (spec * irid) / max(atten, 1e-6);

    vec3 shaded = (vec3(1.0) - exp(-lit * uLightPack[2].w)) * inUV3.w;

    // Exact particles_second.fpo radial starburst: exp(-pow(r, _Glare.z) * _Glare.w) * _Glare.x * _Gamma
    vec2 p = (inUV0.xy - 0.5) * 2.0;
    float r = clamp(length(p), 0.0, 1.0);
    float burst = exp(-pow(max(r, 1e-5), uGlare.z) * uGlare.w) * uGlare.x * uGamma;
    float edgeMask = smoothstep(1.0, 0.85, r);

    outColor = vec4(shaded * uColor.rgb * burst * edgeMask, burst * edgeMask);
}
