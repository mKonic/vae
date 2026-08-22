#version 450

// P2 bring-up shader: proves vertex buffers, push constants, pipeline creation and present.
// The draw layer's real shaders land in P3.

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
} push;

layout(location = 0) out vec4 vColor;

void main() {
    // The viewport is flipped in VulkanCommandList, so positions are authored y-down.
    gl_Position = vec4(aPos * push.scale + push.offset, 0.0, 1.0);
    vColor = aColor;
}
