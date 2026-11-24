#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec3 in_normal;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec3 v_world_pos;

layout(push_constant) uniform RibbonPush {
    mat4 mvp;
    vec4 params0; // x: brightness, y: fresnel, z: falloff, w: time
} pc;

void main() {
    v_uv = in_uv;
    v_normal = normalize(in_normal);
    v_world_pos = in_pos;
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
