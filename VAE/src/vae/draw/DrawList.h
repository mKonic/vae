#pragma once

#include "vae/draw/Paint.h"

#include <vector>

namespace vae::draw {

    // GPU-side instance layouts. std430, so every member is vec4-aligned deliberately — a stray
    // vec2 or float here silently shifts every field after it in the shader.
    struct QuadInstance {
        Vec4 rect;          // x, y, w, h in device pixels
        Vec4 radii;         // tl, tr, br, bl
        Vec4 color0;
        Vec4 color1;
        Vec4 gradient;      // linear: from.xy, to.xy | radial: centre.xy, radius, unused
        Vec4 borderColor;
        Vec4 clipRect;
        Vec4 clipRadii;
        Vec4 params;        // x: border width, y: fill kind, z: texture slot, w: unused
        Vec4 uv;            // u0, v0, u1, v1
    };

    struct ShadowInstance {
        Vec4 rect;
        Vec4 radii;
        Vec4 color;
        Vec4 clipRect;
        Vec4 clipRadii;
        Vec4 params;        // x: blur sigma, yzw unused
    };

    enum class PrimitiveKind : u8 { Quad, Shadow };

    // A run of consecutive same-kind primitives that can be drawn with one instanced call. Runs are
    // cut when the kind changes or when the texture table would overflow, and NEVER reordered:
    // painter order is submission order, because a UI is drawn back-to-front with no depth buffer.
    struct Batch {
        PrimitiveKind kind = PrimitiveKind::Quad;
        u32 first = 0;
        u32 count = 0;
        u32 textureBase = 0;         // index into the draw list's texture table
        u32 textureCount = 0;
    };

    // Records one frame's worth of primitives. Cheap to clear and refill, so a dirty subtree can be
    // re-recorded without touching the GPU until the batches are submitted.
    class DrawList {
    public:
        static constexpr u32 kMaxTexturesPerBatch = 16;

        void Reset();

        // Clips intersect with whatever is already on the stack. Rounded clips do not intersect
        // exactly — the innermost rounded rect wins and the rest degrade to their bounding box,
        // which is honest for the 99% case (one rounded card clipping its contents). Arbitrary
        // nested rounded clipping needs the offscreen layer path.
        void PushClip(const Rect& rect, Corners corners = {});
        void PopClip();

        // Applied at record time on the CPU, so clip rects, corner radii, border widths and
        // antialiasing all stay in device pixels and behave correctly under canvas zoom.
        void PushTransform(Vec2 scale, Vec2 translate);
        void PopTransform();

        void AddRect(const Rect& rect, const Paint& paint,
                     Corners corners = {}, const Stroke& stroke = {});
        void AddShadow(const Rect& rect, const ShadowSpec& shadow, Corners corners = {});
        void AddLine(Vec2 a, Vec2 b, f32 width, Color color);

        // One glyph from an atlas page. `uv` is normalized within that page. The atlas is R8, so
        // the sampled value is coverage and the colour comes entirely from `color`.
        void AddGlyph(const Rect& rect, const Rect& uv, Color color,
                      const Ref<gpu::Texture>& atlasPage);

        const std::vector<QuadInstance>&   Quads()    const { return m_Quads; }
        const std::vector<ShadowInstance>& Shadows()  const { return m_Shadows; }
        const std::vector<Batch>&          Batches()  const { return m_Batches; }
        const std::vector<Ref<gpu::Texture>>& Textures() const { return m_Textures; }
        bool Empty() const { return m_Batches.empty(); }

    private:
        struct ClipState { Rect rect; Corners corners; };
        struct Transform { Vec2 scale{ 1.0f, 1.0f }; Vec2 translate{ 0.0f, 0.0f }; };

        Rect  Apply(const Rect& r) const;
        f32   ApplyScalar(f32 v) const;
        Vec2  ApplyVector(Vec2 v) const;   // scale only, no translation
        Corners ApplyCorners(Corners c) const;
        ClipState CurrentClip() const;
        Batch& OpenBatch(PrimitiveKind kind);   // returns m_Batches.back()
        u32   SlotFor(const Ref<gpu::Texture>& texture, Batch& batch);

        std::vector<QuadInstance>   m_Quads;
        std::vector<ShadowInstance> m_Shadows;
        std::vector<Batch>          m_Batches;
        std::vector<Ref<gpu::Texture>> m_Textures;

        std::vector<ClipState> m_ClipStack;
        std::vector<Transform> m_TransformStack;
    };

}
