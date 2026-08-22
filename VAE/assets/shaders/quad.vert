#version 450

// One instanced quad per primitive. There is no vertex buffer: the six corners are derived from
// gl_VertexIndex and the instance's rect, so adding a primitive costs one struct in a storage
// buffer and nothing else.

struct QuadInstance {
    vec4 rect;
    vec4 radii;
    vec4 color0;
    vec4 color1;
    vec4 gradient;
    vec4 borderColor;
    vec4 clipRect;
    vec4 clipRadii;
    vec4 params;
    vec4 uv;
};

layout(std430, set = 0, binding = 0) readonly buffer Quads { QuadInstance quads[]; };

layout(push_constant) uniform Push { vec2 viewport; } push;

layout(location = 0) out vec2      vLocal;    // px from the rect centre
layout(location = 1) out vec2      vScreen;   // px in device space, for the clip SDF
layout(location = 2) out flat uint vIndex;

// Padding so the antialiased edge and the border are inside the drawn quad.
const float kPad = 1.5;

vec2 CornerOf(int index) {
    // Two triangles: (0,0) (1,0) (0,1) / (0,1) (1,0) (1,1)
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    return corners[index];
}

void main() {
    QuadInstance q = quads[gl_InstanceIndex];

    vec2 halfSize = q.rect.zw * 0.5;
    vec2 centre   = q.rect.xy + halfSize;
    vec2 local    = (CornerOf(gl_VertexIndex) * 2.0 - 1.0) * (halfSize + kPad);
    vec2 screen   = centre + local;

    // Vulkan clip space is already y-down, which matches UI space; no flip anywhere.
    gl_Position = vec4(screen / push.viewport * 2.0 - 1.0, 0.0, 1.0);
    vLocal  = local;
    vScreen = screen;
    vIndex  = gl_InstanceIndex;
}
