#version 450
// Decoded from: lib/moyou/back_colours0.fpo (SCE_FP_RSX)
// Procedural PS3 XMB time-of-day & month-of-year sky color generator.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform BackColoursUBO {
    float uDayTime;
    float uMonthTime;
    float uNightTime;
    float uNightBrightness;
    float uAlpha;
    float uNightDayBlend;
};

layout(set = 0, binding = 1) uniform sampler2D uMonthlyTex1Day;
layout(set = 0, binding = 2) uniform sampler2D uMonthlyTex2Day;
layout(set = 0, binding = 3) uniform sampler2D uMonthlyTex1Night;
layout(set = 0, binding = 4) uniform sampler2D uMonthlyTex2Night;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;

float clampedTanh(float x) {
    float c = clamp(x, -10.0, 10.0);
    float e2 = exp(2.0 * c);
    return (e2 - 1.0) / (e2 + 1.0);
}

vec3 rgb2hsv(vec3 c) {
    float cMax = max(c.r, max(c.g, c.b));
    float cMin = min(c.r, min(c.g, c.b));
    float delta = cMax - cMin;
    if (delta == 0.0) {
        return vec3(0.0, 0.0, cMax);
    }
    vec3 d = ((vec3(cMax) - c) / (6.0 * delta)) + 0.5;
    float h;
    if (c.r == cMax)      h = d.b - d.g;
    else if (c.g == cMax) h = (1.0 / 3.0) + d.r - d.b;
    else                  h = (2.0 / 3.0) + d.g - d.r;
    if (h < 0.0) h += 1.0;
    if (h > 1.0) h -= 1.0;
    return vec3(h, delta / cMax, cMax);
}

vec3 hsv2rgb(vec3 hsv) {
    float h = clamp(hsv.x - 0.0001, 0.0, 1.0) * 6.0;
    float s = hsv.y;
    float v = hsv.z;
    float f = fract(h);
    float p = v * (1.0 - s);
    float q = v * (1.0 - s * f);
    float t = v * (1.0 - s * (1.0 - f));
    int sector = int(floor(h));
    if (sector == 0) return vec3(v, t, p);
    if (sector == 1) return vec3(q, v, p);
    if (sector == 2) return vec3(p, v, t);
    if (sector == 3) return vec3(p, q, v);
    if (sector == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}

void main() {
    // Flip V for Vulkan so y = 0 is bottom horizon (matching PS3 RSX/OpenGL UV orientation)
    vec2 uv = vec2(inUV.x, 1.0 - inUV.y);

    // Quintic smoothstep interpolation on 64x32 monthly palette LUTs
    vec2 grid = uv * vec2(64.0, 32.0) + 0.5;
    vec2 cell = floor(grid);
    vec2 frac = grid - cell;
    vec2 smoothFrac = frac * frac * frac * (frac * (frac * 6.0 - 15.0) + 10.0);
    vec2 lutUV = (cell + smoothFrac - 0.5) * vec2(1.0 / 64.0, 1.0 / 32.0);

    vec3 hsv1 = rgb2hsv(texture(uMonthlyTex1Day, lutUV).rgb);
    vec3 hsv2 = rgb2hsv(texture(uMonthlyTex2Day, lutUV).rgb);

    // Solar & lunar positions across the sky dome
    float angNight = (uNightTime - 600.0) * (PI / 1200.0);
    float angDay   = (uDayTime   - 600.0) * (PI / 1200.0);

    vec2 nightSunPos = vec2(cos(angNight) * 0.8 + uv.x - 0.5, uv.y - sin(angNight) * 0.99);
    vec2 daySunPos   = vec2(cos(angDay)   * 0.8 + uv.x - 0.5, uv.y - sin(angDay)   * 0.99);

    float nightGlow = exp(-8.0 * (1.25 * nightSunPos.y * nightSunPos.y + nightSunPos.x * nightSunPos.x));
    float dayGlow   = exp(-8.0 * (1.25 * daySunPos.y   * daySunPos.y   + daySunPos.x   * daySunPos.x));

    // Horizon glow discs around dusk/dawn (t = 1200)
    float dayDusk   = exp(-0.008 * (uDayTime - 1200.0) * (uDayTime - 1200.0)) + 1.0;
    float nightDusk = exp(-0.008 * (uNightTime - 1200.0) * (uNightTime - 1200.0)) + 1.0;
    float dayHaze   = exp(-4.0 * ((uv.y - 0.05) * (uv.y - 0.05) + 0.25 * pow(uv.x - 0.4 * cos(angDay) - 0.5, 2.0) / dayDusk));
    float nightHaze = exp(-4.0 * ((uv.y - 0.05) * (uv.y - 0.05) + 0.25 * pow(uv.x - 0.4 * cos(angNight) - 0.5, 2.0) / nightDusk));

    // Smooth day/night envelope curves
    float envDay   = 0.25 * (clampedTanh((uDayTime - 600.0) * 0.005) + 1.0)
                   * (clampedTanh((1800.0 - uDayTime) * 0.005) + 1.0);
    float envNight = 0.25 * (clampedTanh((uNightTime - 600.0) * 0.005) + 1.0)
                   * (clampedTanh((1800.0 - uNightTime) * 0.005) + 1.0);
    float envDay2  = 0.25 * (clampedTanh((uDayTime - 600.0) * 0.01) + 1.0)
                   * (clampedTanh((1800.0 - uDayTime) * 0.01) + 1.0);
    float envNight2 = 0.25 * (clampedTanh((uNightTime - 600.0) * 0.01) + 1.0)
                    * (clampedTanh((1800.0 - uNightTime) * 0.01) + 1.0);

    float monthPeak = 0.25 * exp(-0.01 * (uMonthTime - 15.0) * (uMonthTime - 15.0));

    float vGradDay   = exp(uv.y * (-6.4 * (1.0 - envDay)));
    float vGradNight = exp(uv.y * (-6.4 * (1.0 - envNight)));
    float satFactor  = 2.0 - exp(uv.y * (-6.4 * (1.0 - envDay2))) - monthPeak;

    float dayBright   = 0.6 + 0.2 * dayGlow + 0.19 * dayHaze;
    float nightBright = max((0.6 + 0.2 * nightGlow + 0.19 * nightHaze) * vGradNight * envNight2, uNightBrightness);

    float satScale = 0.5 * (3.0 - 2.0 * dayBright) * satFactor;
    vec3 colDay1 = hsv2rgb(vec3(hsv1.x, hsv1.y * satScale, hsv1.z * vGradDay * dayBright * envDay));
    vec3 colDay2 = hsv2rgb(vec3(hsv2.x, hsv2.y * satScale, hsv2.z * vGradDay * dayBright * envDay));

    float mDiff = uMonthTime - 15.0;
    float blendSlope = mDiff * mDiff * (1.0 / 22.5) + 0.1;
    float monthBlend = 0.5 - 0.5 * clampedTanh((uv.y - 0.1 * uMonthTime) * blendSlope + blendSlope);

    vec3 dayCol = mix(colDay1, colDay2, monthBlend);

    vec3 night1 = texture(uMonthlyTex1Night, lutUV).rgb * nightBright;
    vec3 night2 = texture(uMonthlyTex2Night, lutUV).rgb * nightBright;
    vec3 nightCol = mix(night1, night2, monthBlend);

    vec3 finalRGB = mix(nightCol, dayCol, uNightDayBlend);
    outColor = vec4(finalRGB, uAlpha);
}
