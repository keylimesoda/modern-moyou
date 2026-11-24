#version 450
// Decoded from: lib/glutils/ToneMap.fpo + lib/glutils/ToneApplyDisplay.fpo (SCE_FP_RSX)
// Extended Reinhard (2002) tone mapping + optional glare composite and dithering.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform ToneMapUBO {
    float uExposure;
    float uWhiteSqrRcp; // 1.0 / (L_white^2)
    float uGlareWeight; // 0.0 to disable glare add
    float uDitherScale; // stock RSX ToneApplyDisplay uses 0.125 / 255-scale noise
    float uToneBefore;  // 1.0 = TONEBEFORE (tonemap scene before adding glare), 0.0 = add glare before tonemap
    float uFrameSeed;   // animated per-frame dither seed (matches 32x32 animated noise in 0x071794)
    float uSurfaceAlpha;// premultiplied surface opacity (1.0 opaque; <1.0 translucent layer-shell underlay)
    float uHdrMode;     // 0.0 = normal HDR, 1.0 = HDR.ENABLED==0 passthrough, 2.0 = HDR.GLARE_ONLY
};

layout(set = 0, binding = 1) uniform sampler2D uSceneTex;
layout(set = 0, binding = 2) uniform sampler2D uGlareTex;

layout(location = 0) out vec4 outColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 reinhardExtended(vec3 c) {
    vec3 L = c * uExposure;
    return (L * (vec3(1.0) + L * uWhiteSqrRcp)) / (vec3(1.0) + L);
}

void main() {
    vec4 src   = texture(uSceneTex, inUV);
    vec3 glare = texture(uGlareTex, inUV).rgb * uGlareWeight;

    vec3 mapped;
    if (uHdrMode > 1.5) {
        // HDR.GLARE_ONLY == 1
        mapped = glare;
    } else if (uHdrMode > 0.5) {
        // HDR.ENABLED == 0 passthrough
        mapped = src.rgb;
    } else if (uToneBefore > 0.5) {
        // Exact Sony TONEBEFORE = 1 path (0x077398): tonemap scene first, then add 5-mip glare
        mapped = reinhardExtended(src.rgb) + glare;
    } else {
        mapped = reinhardExtended(src.rgb + glare);
    }

    // Animated screen-space dithering (mirrors Sony 32x32 animated hash-noise texture in 0x071794)
    float dither = (hash12(gl_FragCoord.xy + vec2(uFrameSeed * 17.0, uFrameSeed * 31.0)) - 0.5) * uDitherScale;
    vec3 finalRgb = clamp(mapped + vec3(dither), 0.0, 1.0);
    float a = clamp(uSurfaceAlpha, 0.0, 1.0);
    outColor = vec4(finalRgb * a, a);
}
