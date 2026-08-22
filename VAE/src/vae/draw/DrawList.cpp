#include "vaepch.h"
#include "vae/draw/DrawList.h"

#include <cmath>

namespace vae::draw {

    namespace {
        // Stands in for "no clip". Large enough to never cut anything, small enough that squaring
        // it in the shader's SDF cannot overflow a float.
        constexpr f32 kUnclipped = 1.0e6f;
        const Rect kNoClip{ { -kUnclipped * 0.5f, -kUnclipped * 0.5f }, { kUnclipped, kUnclipped } };

        Vec4 ToVec4(const Rect& r) { return { r.pos.x, r.pos.y, r.size.x, r.size.y }; }
    }

    void DrawList::Reset() {
        m_Quads.clear();
        m_Shadows.clear();
        m_Batches.clear();
        m_Textures.clear();
        m_ClipStack.clear();
        m_TransformStack.clear();
    }

    void DrawList::PushTransform(Vec2 scale, Vec2 translate) {
        // Compose with the parent so nested transforms behave (canvas zoom inside a scrolled panel).
        const Transform parent = m_TransformStack.empty() ? Transform{} : m_TransformStack.back();
        Transform t;
        t.scale = parent.scale * scale;
        t.translate = parent.translate + parent.scale * translate;
        m_TransformStack.push_back(t);
    }

    void DrawList::PopTransform() { if (!m_TransformStack.empty()) m_TransformStack.pop_back(); }

    Rect DrawList::Apply(const Rect& r) const {
        if (m_TransformStack.empty()) return r;
        const Transform& t = m_TransformStack.back();
        return Rect{ r.pos * t.scale + t.translate, r.size * t.scale };
    }

    f32 DrawList::ApplyScalar(f32 v) const {
        if (m_TransformStack.empty()) return v;
        const Vec2 s = m_TransformStack.back().scale;
        return v * 0.5f * (s.x + s.y);      // uniform scale in practice; average is the sane answer
    }

    Vec2 DrawList::ApplyVector(Vec2 v) const {
        if (m_TransformStack.empty()) return v;
        return v * m_TransformStack.back().scale;
    }

    Corners DrawList::ApplyCorners(Corners c) const {
        if (m_TransformStack.empty()) return c;
        const f32 s = ApplyScalar(1.0f);
        return Corners{ c.tl * s, c.tr * s, c.br * s, c.bl * s };
    }

    void DrawList::PushClip(const Rect& rect, Corners corners) {
        const Rect transformed = Apply(rect);
        const Corners scaled = ApplyCorners(corners);

        if (m_ClipStack.empty()) {
            m_ClipStack.push_back({ transformed, scaled });
            return;
        }

        const ClipState& parent = m_ClipStack.back();
        ClipState next;
        next.rect = parent.rect.Intersect(transformed);
        // Only one rounded clip can be expressed per instance. The innermost one wins; an outer
        // rounded clip degrades to its bounding box (already applied by the intersection above).
        next.corners = scaled.Any() ? scaled : parent.corners;
        m_ClipStack.push_back(next);
    }

    void DrawList::PopClip() { if (!m_ClipStack.empty()) m_ClipStack.pop_back(); }

    DrawList::ClipState DrawList::CurrentClip() const {
        if (m_ClipStack.empty()) return { kNoClip, Corners{} };
        return m_ClipStack.back();
    }

    Batch& DrawList::OpenBatch(PrimitiveKind kind) {
        if (!m_Batches.empty() && m_Batches.back().kind == kind) return m_Batches.back();

        Batch batch;
        batch.kind = kind;
        batch.first = kind == PrimitiveKind::Quad ? static_cast<u32>(m_Quads.size())
                                                  : static_cast<u32>(m_Shadows.size());
        batch.textureBase = static_cast<u32>(m_Textures.size());
        m_Batches.push_back(batch);
        return m_Batches.back();
    }

    u32 DrawList::SlotFor(const Ref<gpu::Texture>& texture, Batch& batch) {
        for (u32 i = 0; i < batch.textureCount; ++i)
            if (m_Textures[batch.textureBase + i] == texture) return i;

        if (batch.textureCount == kMaxTexturesPerBatch) {
            // Batch is full. Cut it and start a new one rather than silently dropping the image —
            // this is the one place draw order and batching interact, and getting it wrong shows up
            // as a texture from the wrong element.
            Batch next;
            next.kind = batch.kind;
            next.first = batch.first + batch.count;
            next.textureBase = static_cast<u32>(m_Textures.size());
            m_Batches.push_back(next);
            Batch& fresh = m_Batches.back();
            m_Textures.push_back(texture);
            fresh.textureCount = 1;
            return 0;
        }

        m_Textures.push_back(texture);
        return batch.textureCount++;
    }

    void DrawList::AddRect(const Rect& rect, const Paint& paint, Corners corners, const Stroke& stroke) {
        const Rect r = Apply(rect);
        if (r.Empty()) return;

        OpenBatch(PrimitiveKind::Quad);
        const ClipState clip = CurrentClip();

        // SlotFor can append a batch (texture table full), which invalidates any reference held
        // across it — so always re-read m_Batches.back() rather than caching one.
        u32 slot = 0;
        if (paint.kind == Paint::Kind::Image && paint.image)
            slot = SlotFor(paint.image, m_Batches.back());

        QuadInstance q{};
        q.rect        = ToVec4(r);
        q.radii       = ApplyCorners(corners).AsVec4();
        q.color0      = paint.color;
        q.color1      = paint.color1;
        q.gradient    = { paint.from.x, paint.from.y, paint.to.x, paint.to.y };
        q.borderColor = stroke.color;
        q.clipRect    = ToVec4(clip.rect);
        q.clipRadii   = clip.corners.AsVec4();
        q.params      = { ApplyScalar(stroke.width), static_cast<f32>(paint.kind),
                          static_cast<f32>(slot), 0.0f };
        q.uv          = { paint.uv.pos.x, paint.uv.pos.y,
                          paint.uv.pos.x + paint.uv.size.x, paint.uv.pos.y + paint.uv.size.y };

        m_Quads.push_back(q);
        ++m_Batches.back().count;
    }

    void DrawList::AddShadow(const Rect& rect, const ShadowSpec& shadow, Corners corners) {
        Rect r = Apply(rect);
        if (r.Empty()) return;

        const f32 spread = ApplyScalar(shadow.spread);
        const f32 blur   = ApplyScalar(shadow.blur);
        r = r.Inset(-spread, -spread).Translated(ApplyVector(shadow.offset));

        OpenBatch(PrimitiveKind::Shadow);
        const ClipState clip = CurrentClip();

        ShadowInstance s{};
        s.rect      = ToVec4(r);
        s.radii     = ApplyCorners(corners).AsVec4();
        s.color     = shadow.color;
        s.clipRect  = ToVec4(clip.rect);
        s.clipRadii = clip.corners.AsVec4();
        // CSS/Figma "blur radius" is roughly 2 sigma; the shader integrates a Gaussian, so convert
        // once here rather than making every caller think in sigma.
        s.params    = { std::max(blur, 0.0f) * 0.5f, 0.0f, 0.0f, 0.0f };

        m_Shadows.push_back(s);
        ++m_Batches.back().count;
    }

    void DrawList::AddGlyph(const Rect& rect, const Rect& uv, Color color,
                            const Ref<gpu::Texture>& atlasPage) {
        const Rect r = Apply(rect);
        if (r.Empty() || !atlasPage) return;

        OpenBatch(PrimitiveKind::Quad);
        const ClipState clip = CurrentClip();
        const u32 slot = SlotFor(atlasPage, m_Batches.back());

        QuadInstance q{};
        q.rect      = ToVec4(r);
        q.color0    = color;
        q.clipRect  = ToVec4(clip.rect);
        q.clipRadii = clip.corners.AsVec4();
        q.params    = { 0.0f, static_cast<f32>(Paint::Kind::Glyph), static_cast<f32>(slot), 0.0f };
        q.uv        = { uv.pos.x, uv.pos.y, uv.pos.x + uv.size.x, uv.pos.y + uv.size.y };

        m_Quads.push_back(q);
        ++m_Batches.back().count;
    }

    void DrawList::AddLine(Vec2 a, Vec2 b, f32 width, Color color) {
        // Axis-aligned lines are the overwhelmingly common case in UI (dividers, rulers, grid), and
        // they are exactly a thin rect — no rotation, no extra pipeline. Diagonals wait for the
        // path renderer rather than being faked with a stretched quad that antialiases wrong.
        const f32 half = width * 0.5f;
        if (std::abs(a.y - b.y) < 0.01f) {
            AddRect(Rect::FromEdges(std::min(a.x, b.x), a.y - half, std::max(a.x, b.x), a.y + half),
                    Paint::Solid(color));
        } else if (std::abs(a.x - b.x) < 0.01f) {
            AddRect(Rect::FromEdges(a.x - half, std::min(a.y, b.y), a.x + half, std::max(a.y, b.y)),
                    Paint::Solid(color));
        } else {
            VAE_CORE_WARN("AddLine: diagonal lines need the path renderer (P3 follow-up)");
        }
    }

}
