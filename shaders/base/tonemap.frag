#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_hdr_tex;
layout(set = 0, binding = 1) uniform sampler2D u_bloom_tex;

layout(push_constant) uniform TonemapPush {
    vec4 params0; // x: exposure, y: white_level, z: glare_level, w: gamma
    vec4 params1; // x: glare_enabled, y: glare_only, z: dither, w: unused
} pc;

void main() {
    vec3 hdr = texture(u_hdr_tex, v_uv).rgb;
    vec3 bloom = texture(u_bloom_tex, v_uv).rgb;

    vec3 combined = (pc.params1.y > 0.5)
        ? (bloom * pc.params0.z)
        : (hdr + bloom * pc.params0.z * pc.params1.x);

    // Reinhard extended tonemap with HDR.mnu EXPOSURE & WHITE LEVEL
    vec3 exposed = max(combined * pc.params0.x, vec3(0.0));
    float wp = max(pc.params0.y, 0.1);
    float wp2 = wp * wp;
    vec3 mapped = (exposed * (1.0 + exposed / wp2)) / (1.0 + exposed);

    float inv_gamma = 1.0 / max(pc.params0.w, 1.0);
    vec3 ldr = pow(clamp(mapped, 0.0, 1.0), vec3(inv_gamma));
    out_color = vec4(ldr, 1.0);
}
