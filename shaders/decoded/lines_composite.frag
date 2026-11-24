#version 450
// Decoded from: lib/moyou/LinesController.vpo + LinesController.fpo (custom_render_plugin.prx 0x057cf8)
// Composites the linear FP16 additive ribbon buffer (_Tex0) with the 64x32 procedural sky (_Back)
// using BACKGROUND.mnu vertical (1/2) and horizontal (3/4) tint gradients.

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform LinesCompositeUBO {
    vec4 uColour1; // vgradcol at y = 0 (bottom): BACKGROUND 2 RGB * tint
    vec4 uColour2; // vgradcol at y = 1 (top):    BACKGROUND 1 RGB * tint
    vec4 uColour3; // hgradcol at x = 0 (left):   BACKGROUND 3 RGB * tint
    vec4 uColour4; // hgradcol at x = 1 (right):  BACKGROUND 4 RGB * tint
};

layout(set = 0, binding = 1) uniform sampler2D uRibbonTex; // FP16 additive ribbon accumulation
layout(set = 0, binding = 2) uniform sampler2D uSkyTex;    // 64x32 procedural sky (back_colours0)

layout(location = 0) out vec4 outColor;

void main() {
    vec3 sky    = texture(uSkyTex, inUV).rgb;
    float rib   = texture(uRibbonTex, inUV).r;

    vec3 vgrad  = mix(uColour1.rgb, uColour2.rgb, 1.0 - inUV.y);
    vec3 hgrad  = mix(uColour3.rgb, uColour4.rgb, inUV.x);

    // Keep sky gradient within [0..1] (no-op on stock OFW where 1/2 RGB <= 1.0; prevents modded 2.0 sky gain from blowing out bloom)
    vec3 hdr = sky * min(vgrad, vec3(1.0)) + rib * hgrad;
    outColor = vec4(hdr, 1.0);
}
