#version 450
// Decoded from: lib/moyou/ffd_shader0.fpo (SCE_FP_RSX)
// Free-Form Deformation wave displacement shader (variant 0).

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
    float t = uTime + 2400.0;

    float c1 = (sin(0.1 * t) + 0.2) * (1.0 / 1.2);
    float c2 = (sin(0.1 * t + 20.0) + 0.2) * (1.0 / 1.2);

    float d1 = u - c1;
    float d2 = u - c2;
    float bumps = exp(-50.0 * d1 * d1) - 0.15 * exp(-10.0 * d2 * d2);

    float env = (0.5 - v) * (1.0 - exp(-2.0 * u));
    float phase = -6.28 * u + 0.5 * t * (1.0 - 0.8 * u);
    float mainWave = env * sin(phase);
    float ripple = 0.25 * sin(15.7 * u - 0.13 * t - 2.0);

    float dx = 1.31 * u * u + 0.2 * u;
    float dy = bumps + mainWave + ripple;
    outDisp = vec4(dx, dy, 0.0, 0.0);
}
