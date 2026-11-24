#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_blur;

layout(set = 0, binding = 0) uniform sampler2D u_src_tex;

layout(push_constant) uniform BloomBlurPush {
    vec2 direction; // (1/w, 0) or (0, 1/h)
    vec2 pad;
    vec4 sigma_rgb; // x: rad_r, y: rad_g, z: rad_b, w: unused
} pc;

void main() {
    // Separable per-channel RGB Gaussian blur matching HDR.mnu GAUSSIAN RAD R/G/B
    vec3 sigma = max(pc.sigma_rgb.rgb, vec3(0.25));
    vec3 inv_2s2 = 1.0 / (2.0 * sigma * sigma);

    vec3 sum = vec3(0.0);
    vec3 norm = vec3(0.0);

    for (int i = -6; i <= 6; ++i) {
        float fi = float(i);
        vec3 w = exp(-(fi * fi) * inv_2s2);
        vec3 s = texture(u_src_tex, v_uv + pc.direction * fi).rgb;
        sum += s * w;
        norm += w;
    }

    out_blur = vec4(sum / max(norm, vec3(1e-5)), 1.0);
}
