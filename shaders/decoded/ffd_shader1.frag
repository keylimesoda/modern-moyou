#version 450
// Decoded from: lib/moyou/ffd_shader1.fpo (SCE_FP_RSX)
// Free-Form Deformation wave displacement shader (variant 1).

layout(location = 0) in vec2 inUV;

layout(set = 0, binding = 0) uniform FfdUBO {
    float uTime;
};

layout(set = 0, binding = 1) uniform sampler2D uUVW;

layout(location = 0) out vec4 outDisp;

void main() {
    vec2 uv = texture(uUVW, inUV).xy;
    float u = uv.x;
    float v = uv.y;
    float t = uTime;

    float e2 = exp(2.0 * (u - 0.5));
    float tanhU = (e2 - 1.0) / (e2 + 1.0);

    float decay = exp(-0.0001 * t);
    float breathe = (sin(2.0 * t) + 3.0) * (0.24 * tanhU + 0.24) * decay;
    float wave = breathe * sin(7.85 * u - 2.5 * t - 1.25);

    float c = (sin(0.1 * t) + 0.2) * (1.0 / 1.2);
    float d = u - c;
    float bump = exp(-50.0 * d * d);

    float sway = 0.5 * sin(0.25 * t);
    float vRipple = 0.125 * sin(t - 6.28 * v);

    float dx = 1.3 * u * u + 0.2 * u - 0.15;
    float dy = wave + sway + bump + vRipple;
    outDisp = vec4(dx, dy, 0.0, 0.0);
}
