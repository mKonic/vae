#version 450

struct ShadowInstance {
    vec4 rect;
    vec4 radii;
    vec4 color;
    vec4 clipRect;
    vec4 clipRadii;
    vec4 params;        // sigma, unused
};

layout(std430, set = 0, binding = 0) readonly buffer Shadows { ShadowInstance shadows[]; };
layout(push_constant) uniform Push { vec2 viewport; } push;

layout(location = 0) out vec2      vLocal;
layout(location = 1) out vec2      vScreen;
layout(location = 2) out flat uint vIndex;

vec2 CornerOf(int index) {
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    return corners[index];
}

void main() {
    ShadowInstance s = shadows[gl_InstanceIndex];

    // The quad has to cover the whole blur falloff, not just the rect. Three sigma captures
    // ~99.7% of a Gaussian, which is where the shader's integration is truncated too.
    float pad = s.params.x * 3.0 + 2.0;

    vec2 halfSize = s.rect.zw * 0.5;
    vec2 centre   = s.rect.xy + halfSize;
    vec2 local    = (CornerOf(gl_VertexIndex) * 2.0 - 1.0) * (halfSize + pad);
    vec2 screen   = centre + local;

    gl_Position = vec4(screen / push.viewport * 2.0 - 1.0, 0.0, 1.0);
    vLocal  = local;
    vScreen = screen;
    vIndex  = gl_InstanceIndex;
}
