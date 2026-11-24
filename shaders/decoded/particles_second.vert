#version 450
// Decoded from: lib/particles/particles_second.vpo (SCE_VP_RSX)
// Secondary PS3 XMB particle sparkle/starburst pass: scales front-facing billboard
// by specular peak pow(|dot(N, H)|, spec_pow) * uGlare.y.

layout(location = 0) in vec4 inPos; // xyz = world center, w = base alpha
layout(location = 1) in vec2 inUV0; // quad corner [0, 1]^2
layout(location = 2) in vec4 inRot; // orientation quaternion (x, y, z, w)

layout(set = 0, binding = 0) uniform ParticlesVertexUBO {
    mat4 uModelviewProjection;
    mat4 uModelview;
    mat4 uLightPack;
    vec4 uParticleSize;
    vec4 uTransparency;
    vec4 uFocus;
    vec4 uFocusCurves;
    vec4 uFrontFacingQuaternion;
    vec4 uNearControl;
    vec4 uLifeBoundsMin;
    vec4 uLifeBoundsMax;
    vec4 uDarkness;
    vec4 uGlare; // x = glare, y = glare_scale, z = glare_p1, w = glare_p2
};

layout(location = 0) out vec4 outUV0;
layout(location = 1) out vec4 outUV2;
layout(location = 2) out vec4 outUV3;
layout(location = 3) out vec3 outNormal;
layout(location = 4) out vec4 outWorldPos;
layout(location = 5) out vec4 outNormalV;

mat3 quatToMat3(vec4 q) {
    vec3 q2 = q.xyz * 2.0;
    float xx = q.x * q2.x, yy = q.y * q2.y, zz = q.z * q2.z;
    float xy = q.x * q2.y, xz = q.x * q2.z, yz = q.y * q2.z;
    float wx = q.w * q2.x, wy = q.w * q2.y, wz = q.w * q2.z;
    return mat3(
        vec3(1.0 - (yy + zz), xy + wz,         xz - wy),
        vec3(xy - wz,         1.0 - (xx + zz), yz + wx),
        vec3(xz + wy,         yz - wx,         1.0 - (xx + yy))
    );
}

void main() {
    vec3 dBounds = min(inPos.xyz - uLifeBoundsMin.xyz, uLifeBoundsMax.xyz - inPos.xyz);
    float tLife = clamp(min(dBounds.x, min(dBounds.y, dBounds.z)) * 5.0, 0.0, 1.0);
    float lifeFade = tLife * tLife * (3.0 - 2.0 * tLife);

    vec4 q = normalize(inRot);
    mat3 baseBasis = quatToMat3(q);
    vec3 baseN = baseBasis[2];
    outNormal  = baseN;
    outNormalV = uModelview * vec4(baseN, 0.0);

    vec3 eyePos   = vec3(uLightPack[0].x, uLightPack[1].x, uLightPack[2].x);
    vec3 lightPos = vec3(uLightPack[0].y, uLightPack[1].y, uLightPack[2].y);

    vec3 toEye = eyePos - inPos.xyz;
    float dist = length(toEye);
    vec3 V = toEye / max(dist, 1e-6);
    vec3 L = normalize(lightPos - inPos.xyz);
    vec3 H = normalize(L + V);

    float nearDenom = uFocus.x - uFocus.y;
    float nearT = clamp((dist - uFocus.y) / (abs(nearDenom) > 1e-6 ? nearDenom : -1e-6), 0.0, 1.0);
    float farDenom = uFocus.w - uFocus.z;
    float farT  = clamp((dist - uFocus.z) / (abs(farDenom) > 1e-6 ? farDenom : 1e-6), 0.0, 1.0);
    float nearBlur = pow(nearT, uFocusCurves.x);
    float farBlur  = pow(farT,  uFocusCurves.y);

    float baseSize = mix(mix(uParticleSize.x, uParticleSize.y, nearBlur), uParticleSize.z, farBlur);
    float specPeak = pow(max(abs(dot(baseN, H)), 1e-6), uLightPack[3].y);
    float size = baseSize * (1.0 + specPeak * uGlare.y);

    float nearBand0 = uFocus.z - 0.2;
    float nearBand1 = uFocus.z - 0.4;
    float nearBand2 = uFocus.z - 0.6;
    float rampUp   = clamp((dist - nearBand0) / max(uFocus.z - nearBand0, 1e-6), 0.0, 1.0);
    float rampDown = clamp((dist - nearBand2) / max(nearBand1 - nearBand2, 1e-6), 0.0, 1.0);
    float nearAlpha = 1.0 + rampUp - rampDown;

    float angularSize = 2.0 * atan(baseSize / max(dist, 1e-6)) / max(uFocusCurves.z, 1e-6);

    // Front-facing billboard quad for starburst glare
    mat3 billBasis = quatToMat3(normalize(uFrontFacingQuaternion));
    vec2 corner = inUV0 - 0.5;
    vec4 worldPos = vec4(inPos.xyz + (billBasis[0] * corner.x + billBasis[1] * corner.y) * size, 1.0);

    float facing = 1.0 - pow(clamp(1.0 - abs(dot(V, billBasis[2])), 0.0, 1.0), uTransparency.x);
    float nearDark = 1.0 - clamp(nearT * angularSize * uDarkness.x, 0.0, 1.0);
    float farDark  = 1.0 - clamp(farT  * angularSize * uDarkness.y, 0.0, 1.0);
    float alpha = facing * uTransparency.y * inPos.w * nearAlpha * nearDark * farDark * lifeFade;
    if (inPos.z > eyePos.z - 0.5 || uGlare.x <= 1e-5) {
        alpha = 0.0;
    }

    gl_Position = uModelviewProjection * worldPos;
    outWorldPos = worldPos;
    outUV0 = vec4(inUV0, 0.0, 0.0);
    outUV2 = vec4(nearBlur, farBlur, 0.0, angularSize);
    outUV3 = vec4(1.0, 1.0, 1.0, alpha);
}
