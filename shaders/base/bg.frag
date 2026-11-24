#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_hdr;

layout(set = 0, binding = 0) uniform sampler2D u_day_lut;
layout(set = 0, binding = 1) uniform sampler2D u_night_lut;

layout(push_constant) uniform BgPush {
    vec4 color1;       // rgb + night_blend
    vec4 color2;       // rgb + day_time_u
    vec4 color3;       // rgb + night_white_bias
    vec4 color4;       // rgb + time
} pc;

void main() {
    vec2 lut_uv = vec2(clamp(pc.color2.w, 0.0, 1.0), clamp(v_uv.y, 0.0, 1.0));
    vec3 day_col   = texture(u_day_lut, lut_uv).rgb;
    vec3 night_col = texture(u_night_lut, lut_uv).rgb;

    float night_blend = clamp(pc.color1.w, 0.0, 1.0);
    vec3 base = mix(day_col, night_col, night_blend);

    // Subtle 4-corner tint from BACKGROUND.mnu
    vec3 top_tint = mix(pc.color1.rgb, pc.color2.rgb, v_uv.x);
    vec3 bot_tint = mix(pc.color3.rgb, pc.color4.rgb, v_uv.x);
    vec3 tint = mix(top_tint, bot_tint, v_uv.y);

    // Output linear HDR radiance
    vec3 hdr = pow(max(base * tint, vec3(0.0)), vec3(2.2));
    out_hdr = vec4(hdr, 1.0);
}
