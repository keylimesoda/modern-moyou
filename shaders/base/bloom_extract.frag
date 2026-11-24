#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_glare;

layout(set = 0, binding = 0) uniform sampler2D u_hdr_tex;

layout(push_constant) uniform BloomExtractPush {
    vec4 params; // x: glare_thresh, y: glare_sum_pow, z: exposure, w: white_level
} pc;

void main() {
    vec3 hdr = texture(u_hdr_tex, v_uv).rgb;
    float lum = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
    float soft = max(lum - pc.params.x, 0.0);
    float weight = (lum > 1e-5) ? (soft / lum) : 0.0;
    vec3 extracted = hdr * pow(max(weight, 0.0), max(pc.params.y, 0.1));
    out_glare = vec4(extracted, 1.0);
}
