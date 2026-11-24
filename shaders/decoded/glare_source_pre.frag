#version 450
// Decoded from: lib/glutils/GlareSourcePre.fpo (custom_render_plugin.prx 0x079818)
// 4-tap bilinear downsample + glare threshold extraction for the 5-mip HDR glare pyramid.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform GlareSourceUBO {
    vec2 uTexelOffset; // (±0.5/w, ±0.5/h)
    float uThreshold;  // HDR.mnu GLARE THRESH
    float uExposure;   // HDR.mnu EXPOSURE
};

layout(set = 0, binding = 1) uniform sampler2D uSrcTex;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 o = uTexelOffset;
    vec3 c = 0.25 * (
        texture(uSrcTex, inUV + vec2(-o.x, -o.y)).rgb +
        texture(uSrcTex, inUV + vec2( o.x, -o.y)).rgb +
        texture(uSrcTex, inUV + vec2(-o.x,  o.y)).rgb +
        texture(uSrcTex, inUV + vec2( o.x,  o.y)).rgb
    );
    vec3 glare = max(c * uExposure - vec3(uThreshold), vec3(0.0));
    float lum = dot(glare, vec3(0.27, 0.67, 0.06));
    outColor = vec4(glare, lum);
}
