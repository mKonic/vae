#version 450

// Analytic blurred rounded rectangle. A real Gaussian blur would need the shape rendered to a
// texture and two blur passes per shadow; this closed-form approximation costs one instanced quad.
// Method: a box blurred by a Gaussian has an exact erf solution per axis, so we integrate the
// horizontal solution over a handful of vertical samples. (Evan Wallace's derivation.)

struct ShadowInstance {
    vec4 rect;
    vec4 radii;
    vec4 color;
    vec4 clipRect;
    vec4 clipRadii;
    vec4 params;
};

layout(std430, set = 0, binding = 0) readonly buffer Shadows { ShadowInstance shadows[]; };

layout(location = 0) in vec2      vLocal;
layout(location = 1) in vec2      vScreen;
layout(location = 2) in flat uint vIndex;

layout(location = 0) out vec4 outColor;

float Gaussian(float x, float sigma) {
    const float kInvSqrtTau = 0.39894228;   // 1 / sqrt(2*pi)
    return exp(-(x * x) / (2.0 * sigma * sigma)) * kInvSqrtTau / sigma;
}

// Abramowitz & Stegun style rational approximation of erf, vectorized over two samples.
vec2 Erf(vec2 x) {
    vec2 s = sign(x);
    vec2 a = abs(x);
    vec2 t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t = t * t;
    return s - s / (t * t);
}

float ShadowX(float x, float y, float sigma, float corner, vec2 halfSize) {
    float delta = min(halfSize.y - corner - abs(y), 0.0);
    float curved = halfSize.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    vec2 integral = 0.5 + 0.5 * Erf((x + vec2(-curved, curved)) * (sqrt(0.5) / sigma));
    return integral.y - integral.x;
}

float sdRoundBox(vec2 p, vec2 b, vec4 r) {
    float radius = (p.x > 0.0) ? ((p.y > 0.0) ? r.z : r.y)
                               : ((p.y > 0.0) ? r.w : r.x);
    radius = min(radius, min(b.x, b.y));
    vec2 q = abs(p) - b + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

float Coverage(float distance) {
    float width = fwidth(distance);
    return clamp(0.5 - distance / max(width, 1e-5), 0.0, 1.0);
}

void main() {
    ShadowInstance s = shadows[vIndex];

    vec2 halfSize = s.rect.zw * 0.5;
    float sigma = max(s.params.x, 0.0001);

    // The approximation takes a single corner radius; shadows are soft enough that per-corner
    // radii are not distinguishable, so use the largest to avoid a visible bulge.
    float corner = min(max(max(s.radii.x, s.radii.y), max(s.radii.z, s.radii.w)),
                       min(halfSize.x, halfSize.y));

    float low  = vLocal.y - halfSize.y;
    float high = vLocal.y + halfSize.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end   = clamp( 3.0 * sigma, low, high);

    float step = (end - start) / 4.0;
    float y = start + step * 0.5;
    float value = 0.0;
    for (int i = 0; i < 4; ++i) {
        value += ShadowX(vLocal.x, vLocal.y - y, sigma, corner, halfSize) * Gaussian(y, sigma) * step;
        y += step;
    }

    vec2 clipHalf   = s.clipRect.zw * 0.5;
    vec2 clipCentre = s.clipRect.xy + clipHalf;
    value *= Coverage(sdRoundBox(vScreen - clipCentre, clipHalf, s.clipRadii));

    outColor = vec4(s.color.rgb, s.color.a * clamp(value, 0.0, 1.0));
}
