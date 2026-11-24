#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_old_pos;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in float in_rot;
layout(location = 4) in float in_life;
layout(location = 5) in float in_size;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out float v_life;

layout(push_constant) uniform ParticlePush {
    mat4 mvp;
    vec4 params0; // x: global_alpha, y: exposure, z: glare, w: time
} pc;

void main() {
    v_uv = in_uv;
    v_life = in_life;

    // Placeholder motion-stretch hint using old_pos & rotation
    vec3 vel = in_pos - in_old_pos;
    float c = cos(in_rot);
    float s = sin(in_rot);
    vec2 centered = (in_uv - 0.5) * 2.0;
    vec2 rotated = vec2(c * centered.x - s * centered.y, s * centered.x + c * centered.y);
    vec3 world_pos = in_pos + vec3(rotated * in_size, 0.0) + vel * 0.05;

    gl_Position = pc.mvp * vec4(world_pos, 1.0);
}
