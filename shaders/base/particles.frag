#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in float v_life;

layout(location = 0) out vec4 out_hdr;

layout(push_constant) uniform ParticlePush {
    mat4 mvp;
    vec4 params0; // x: global_alpha, y: exposure, z: glare, w: time
} pc;

void main() {
    vec2 d = (v_uv - 0.5) * 2.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) {
        discard;
    }

    float glow = exp(-r2 * 4.0);
    float fade = sin(clamp(v_life, 0.0, 1.0) * 3.14159265);
    float intensity = glow * fade * pc.params0.x * (1.0 + pc.params0.z * 2.0);
    vec3 col = vec3(1.0, 0.97, 0.92) * intensity;
    out_hdr = vec4(col, intensity);
}
