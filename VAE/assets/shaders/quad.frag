#version 450
#extension GL_EXT_nonuniform_qualifier : require

struct QuadInstance {
    vec4 rect;
    vec4 radii;         // tl, tr, br, bl
    vec4 color0;
    vec4 color1;
    vec4 gradient;
    vec4 borderColor;
    vec4 clipRect;
    vec4 clipRadii;
    vec4 params;        // border width, fill kind, texture slot, unused
    vec4 uv;
};

layout(std430, set = 0, binding = 0) readonly buffer Quads { QuadInstance quads[]; };
layout(set = 0, binding = 1) uniform sampler2D uTextures[16];

layout(location = 0) in vec2      vLocal;
layout(location = 1) in vec2      vScreen;
layout(location = 2) in flat uint vIndex;

layout(location = 0) out vec4 outColor;

const uint kSolid  = 0u;
const uint kLinear = 1u;
const uint kRadial = 2u;
const uint kImage  = 3u;
const uint kGlyph  = 4u;

// Signed distance to a rounded box with independent per-corner radii. p is relative to the centre,
// b is the half-size. Radii arrive in CSS order (tl, tr, br, bl); y grows downward.
float sdRoundBox(vec2 p, vec2 b, vec4 r) {
    float radius = (p.x > 0.0) ? ((p.y > 0.0) ? r.z : r.y)
                               : ((p.y > 0.0) ? r.w : r.x);
    radius = min(radius, min(b.x, b.y));
    vec2 q = abs(p) - b + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

// Screen-space antialiasing: one pixel of coverage regardless of how the shape was scaled.
float Coverage(float distance) {
    float width = fwidth(distance);
    return clamp(0.5 - distance / max(width, 1e-5), 0.0, 1.0);
}

void main() {
    QuadInstance q = quads[vIndex];

    vec2 halfSize = q.rect.zw * 0.5;
    float outer = sdRoundBox(vLocal, halfSize, q.radii);
    float alpha = Coverage(outer);
    if (alpha <= 0.0) discard;

    // Clip, evaluated in device space so it is unaffected by this shape's own transform.
    vec2 clipHalf   = q.clipRect.zw * 0.5;
    vec2 clipCentre = q.clipRect.xy + clipHalf;
    float clipD = sdRoundBox(vScreen - clipCentre, clipHalf, q.clipRadii);
    alpha *= Coverage(clipD);
    if (alpha <= 0.0) discard;

    // Normalized position within the shape, for gradients and image UVs.
    vec2 t = vLocal / max(halfSize, vec2(1e-5)) * 0.5 + 0.5;

    uint kind = uint(q.params.y + 0.5);
    vec4 fill;
    if (kind == kLinear) {
        vec2 axis = q.gradient.zw - q.gradient.xy;
        float denom = max(dot(axis, axis), 1e-5);
        fill = mix(q.color0, q.color1, clamp(dot(t - q.gradient.xy, axis) / denom, 0.0, 1.0));
    } else if (kind == kRadial) {
        float radius = max(q.gradient.z, 1e-5);
        fill = mix(q.color0, q.color1, clamp(length(t - q.gradient.xy) / radius, 0.0, 1.0));
    } else if (kind == kImage) {
        vec2 uv = mix(q.uv.xy, q.uv.zw, t);
        fill = texture(uTextures[nonuniformEXT(uint(q.params.z + 0.5))], uv) * q.color0;
    } else if (kind == kGlyph) {
        // The atlas is single-channel coverage; the glyph's colour is entirely the instance's.
        vec2 uv = mix(q.uv.xy, q.uv.zw, t);
        float coverage = texture(uTextures[nonuniformEXT(uint(q.params.z + 0.5))], uv).r;
        fill = vec4(q.color0.rgb, q.color0.a * coverage);
    } else {
        fill = q.color0;
    }

    // Border occupies the outermost `width` pixels; the fill starts inside it.
    float border = q.params.x;
    if (border > 0.0) {
        float inner = outer + border;
        fill = mix(q.borderColor, fill, Coverage(inner));
    }

    outColor = vec4(fill.rgb, fill.a * alpha);
}
