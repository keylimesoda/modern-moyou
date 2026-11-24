#version 450
// Decoded from: lib/particles/particles_quads.vpo (SCE_VP_RSX)
// PS3 XMB floating bokeh particles with DoF CoC sizing, quaternion billboarding,
// and life-bounds / view-angle alpha attenuation.

layout(location = 0) in vec4 inPos; // xyz = world center, w = base alpha
layout(location = 1) in vec2 inUV0; // quad corner [0, 1]^2
layout(location = 2) in vec4 inRot; // orientation quaternion (x, y, z, w)

layout(set = 0, binding = 0) uniform ParticlesVertexUBO {
    mat4 uModelviewProjection;
    mat4 uModelview;
    mat4 uLightPack;              // col0.xyz = eyePos, col1.xyz = lightPos
    vec4 uParticleSize;           // x = inFocus, y = nearBlur, z = farBlur
    vec4 uTransparency;           // x = facingPow, y = globalAlpha
    vec4 uFocus;                  // x = nearStart, y = nearEnd, z = focusPlane, w = farEnd
    vec4 uFocusCurves;            // x = nearPow, y = farPow, z = atanScale
    vec4 uFrontFacingQuaternion;
    vec4 uNearControl;            // x = angularBlend, y = nearHalo, z = nearBlurBlend
    vec4 uLifeBoundsMin;
    vec4 uLifeBoundsMax;
    vec4 uDarkness;               // x = nearDarkness, y = farDarkness
};

layout(location = 0) out vec4 outUV0;
layout(location = 1) out vec4 outUV2;      // x = nearBlur, y = farBlur, z = nearFade, w = angularSize
layout(location = 2) out vec4 outUV3;      // xyz = 1.0, w = combined vertex alpha
layout(location = 3) out vec3 outNormal;
layout(location = 4) out vec4 outWorldPos;
layout(location = 5) out vec4 outNormalV;

mat3 quatToMat3(vec4 q) {
    vec3 q2 = q.xyz * 2.0;
    float xx = q.x * q2.x;
    float yy = q.y * q2.y;
    float zz = q.z * q2.z;
    float xy = q.x * q2.y;
    float xz = q.x * q2.z;
    float yz = q.y * q2.z;
    float wx = q.w * q2.x;
    float wy = q.w * q2.y;
    float wz = q.w * q2.z;

    return mat3(
        vec3(1.0 - (yy + zz), xy + wz,         xz - wy),
        vec3(xy - wz,         1.0 - (xx + zz), yz + wx),
        vec3(xz + wy,         yz - wx,         1.0 - (xx + yy))
    );
}

void main() {
    // 1. Life bounds smoothstep fade
    vec3 dBounds = min(inPos.xyz - uLifeBoundsMin.xyz, uLifeBoundsMax.xyz - inPos.xyz);
    float tLife = clamp(min(dBounds.x, min(dBounds.y, dBounds.z)) * 5.0, 0.0, 1.0);
    float lifeFade = tLife * tLife * (3.0 - 2.0 * tLife);

    // 2. Base particle normal from quaternion Z-axis
    vec4 q = normalize(inRot);
    mat3 baseBasis = quatToMat3(q);
    vec3 baseN = baseBasis[2];
    outNormal  = baseN;
    outNormalV = uModelview * vec4(baseN, 0.0);

    // 3. Distance to camera & DoF blur factors
    vec3 eyePos = vec3(uLightPack[0].x, uLightPack[1].x, uLightPack[2].x);
    vec3 toEye = eyePos - inPos.xyz;
    float dist = length(toEye);
    vec3 V = toEye / max(dist, 1e-6);

    float nearDenom = uFocus.x - uFocus.y;
    float nearT = clamp((dist - uFocus.y) / (abs(nearDenom) > 1e-6 ? nearDenom : -1e-6), 0.0, 1.0);
    float farDenom = uFocus.w - uFocus.z;
    float farT  = clamp((dist - uFocus.z) / (abs(farDenom) > 1e-6 ? farDenom : 1e-6), 0.0, 1.0);
    float nearBlur = pow(nearT, uFocusCurves.x);
    float farBlur  = pow(farT,  uFocusCurves.y);

    float size0 = mix(uParticleSize.x, uParticleSize.y, nearBlur);
    float size  = mix(size0, uParticleSize.z, farBlur);

    // Near-plane fade ramp around uFocus.z - 0.6 .. uFocus.z
    float nearBand0 = uFocus.z - 0.2;
    float nearBand1 = uFocus.z - 0.4;
    float nearBand2 = uFocus.z - 0.6;
    float rampUp   = clamp((dist - nearBand0) / max(uFocus.z - nearBand0, 1e-6), 0.0, 1.0);
    float rampDown = clamp((dist - nearBand2) / max(nearBand1 - nearBand2, 1e-6), 0.0, 1.0);
    float nearAlpha = 1.0 + rampUp - rampDown;
    float nearFade  = clamp((dist - nearBand1) / max(nearBand0 - nearBand1, 1e-6), 0.0, 1.0);

    // Angular size (replaces RSX 11th-order minimax atan polynomial)
    float angularSize = 2.0 * atan(size / max(dist, 1e-6)) / max(uFocusCurves.z, 1e-6);

    // 4. Quaternion nlerp toward front-facing billboard when close / large
    float billBlend = clamp(nearFade + nearBlur * uNearControl.z + angularSize * uNearControl.x, 0.0, 1.0);
    vec4 qFinal = normalize(mix(q, uFrontFacingQuaternion, billBlend));
    mat3 quadBasis = quatToMat3(qFinal);

    vec2 corner = inUV0 - 0.5;
    vec4 worldPos = vec4(inPos.xyz + (quadBasis[0] * corner.x + quadBasis[1] * corner.y) * size, 1.0);

    // 5. View-angle facing transparency & darkness attenuation
    float facing = 1.0 - pow(clamp(1.0 - abs(dot(V, quadBasis[2])), 0.0, 1.0), uTransparency.x);
    float nearDark = 1.0 - clamp(nearT * angularSize * uDarkness.x, 0.0, 1.0);
    float farDark  = 1.0 - clamp(farT  * angularSize * uDarkness.y, 0.0, 1.0);
    float alpha = facing * uTransparency.y * inPos.w * nearAlpha * nearDark * farDark * lifeFade;
    if (inPos.z > eyePos.z - 0.5) {
        alpha = 0.0;
    }

    gl_Position = uModelviewProjection * worldPos;
    outWorldPos = worldPos;
    outUV0 = vec4(inUV0, 0.0, 0.0);
    outUV2 = vec4(nearBlur, farBlur, nearFade, angularSize);
    outUV3 = vec4(1.0, 1.0, 1.0, alpha);
}
