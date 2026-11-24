#version 450
// Decoded from: lib/particles/particles_quads.fpo (SCE_FP_RSX)
// Iridescent Blinn-Phong bokeh disc particle fragment shader.

layout(location = 0) in vec4 inUV0;
layout(location = 1) in vec4 inUV2;      // x = nearBlur, y = farBlur, z = nearFade, w = angularSize
layout(location = 2) in vec4 inUV3;      // w = vertex alpha
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inWorldPos;
layout(location = 5) in vec4 inNormalV;

layout(set = 0, binding = 1) uniform ParticlesFragUBO {
    mat4 uLightPack;       // col0.xyz=eyePos, col1.xyz=lightPos, col2.xyz=atten(c0,c1,c2),
                           // row0.w=diffStr, row1.w=specStr, row2.w=toneScale, row3.y=specExp, row3.w=iridStr
    vec4 uColor;           // rgb = tint
    vec4 uNearControl;     // y = nearHaloK
    float uIridescentExp;
    float uGamma;
    vec2 _pad0;
};

layout(set = 0, binding = 2) uniform sampler2D uIridescentTex;

layout(location = 0) out vec4 outColor;

void main() {
    // 1. Iridescent normal-map tint
    vec3 iridTex = texture(uIridescentTex, inNormalV.xy * 0.5 + 0.5).rgb;
    float iridStr = uLightPack[3].w;
    vec3 irid = pow(max(vec3(1.0) + (iridTex - vec3(1.0)) * iridStr, vec3(1e-5)), vec3(uIridescentExp));

    // 2. Blinn-Phong lighting with quadratic attenuation
    vec3 eyePos   = vec3(uLightPack[0].x, uLightPack[1].x, uLightPack[2].x);
    vec3 lightPos = vec3(uLightPack[0].y, uLightPack[1].y, uLightPack[2].y);

    vec3 V = normalize(eyePos - inWorldPos.xyz);
    vec3 toLight = lightPos - inWorldPos.xyz;
    float distL = length(toLight);
    vec3 L = toLight / max(distL, 1e-6);
    vec3 H = normalize(L + V);

    float spec = pow(max(abs(dot(inNormal, H)), 1e-6), uLightPack[3].y) * uLightPack[1].w;
    float diff = abs(dot(inNormal, L)) * uLightPack[0].w;

    float atten = uLightPack[0].z + distL * (uLightPack[1].z + distL * uLightPack[2].z);
    vec3 lit = (spec * irid + vec3(diff)) / max(atten, 1e-6);

    // Exponential tone compression per particle
    vec3 shaded = vec3(1.0) - exp(-lit * uLightPack[2].w);

    // 3. Soft DoF bokeh disc + Gaussian halo
    vec2 p = (inUV0.xy - 0.5) * 2.0;
    float r = length(p);
    float blur = clamp(inUV2.x + inUV2.y, 0.0, 1.0);
    float softness = blur * 0.6345 + 0.05;
    float disc = 1.0 - smoothstep(0.5 - softness, 0.5 + softness, r);

    float haloK = mix(0.85, uNearControl.y, inUV2.x);
    float shape = mix(disc, 1.0 - exp(-disc * haloK), blur);
    float alpha = shape * inUV3.w;

    outColor = vec4(shaded * alpha * uColor.rgb * uGamma, alpha);
}
