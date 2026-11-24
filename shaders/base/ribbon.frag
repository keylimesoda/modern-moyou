#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec3 v_world_pos;

layout(location = 0) out vec4 out_hdr;

layout(push_constant) uniform RibbonPush {
    mat4 mvp;
    vec4 params0; // x: brightness, y: fresnel, z: falloff, w: time
} pc;

void main() {
    vec3 N = normalize(v_normal);
    vec3 V = normalize(vec3(0.0, 0.0, 5.0) - v_world_pos);
    float ndotv = abs(dot(N, V));

    float fresnel = pow(1.0 - clamp(ndotv, 0.0, 1.0), max(pc.params0.y * 3.0, 0.25));
    float edge_fade = sin(clamp(v_uv.y, 0.0, 1.0) * 3.14159265);
    float alpha = clamp((0.12 + 0.88 * fresnel) * pow(edge_fade, max(pc.params0.z, 0.1)), 0.0, 1.0);

    vec3 color = vec3(0.85, 0.93, 1.0) * (pc.params0.x * (0.6 + 1.4 * fresnel));
    out_hdr = vec4(color * alpha, alpha);
}
