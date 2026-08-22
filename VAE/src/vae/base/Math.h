#pragma once

#include "vae/base/Base.h"

#include <glm/glm.hpp>
#include <algorithm>

namespace vae {

    using Vec2 = glm::vec2;
    using Vec3 = glm::vec3;
    using Vec4 = glm::vec4;
    using Color = glm::vec4;      // straight (non-premultiplied) alpha, sRGB authoring space

    // Axis-aligned rectangle in layout/paint space: y grows downward, like every UI system and
    // unlike the game-engine convention. Stored as position+size because that is what layout
    // produces and what the renderer instances consume.
    struct Rect {
        Vec2 pos{0.0f};
        Vec2 size{0.0f};

        constexpr f32 Left()   const { return pos.x; }
        constexpr f32 Top()    const { return pos.y; }
        constexpr f32 Right()  const { return pos.x + size.x; }
        constexpr f32 Bottom() const { return pos.y + size.y; }
        constexpr Vec2 Center() const { return pos + size * 0.5f; }

        constexpr bool Contains(Vec2 p) const {
            return p.x >= pos.x && p.y >= pos.y && p.x < pos.x + size.x && p.y < pos.y + size.y;
        }
        constexpr bool Empty() const { return size.x <= 0.0f || size.y <= 0.0f; }

        static Rect FromEdges(f32 l, f32 t, f32 r, f32 b) { return Rect{ {l, t}, {r - l, b - t} }; }

        Rect Intersect(const Rect& o) const {
            const f32 l = std::max(Left(),   o.Left());
            const f32 t = std::max(Top(),    o.Top());
            const f32 r = std::min(Right(),  o.Right());
            const f32 b = std::min(Bottom(), o.Bottom());
            return (r <= l || b <= t) ? Rect{} : FromEdges(l, t, r, b);
        }
        Rect Inset(f32 dx, f32 dy) const { return FromEdges(Left() + dx, Top() + dy, Right() - dx, Bottom() - dy); }
        Rect Translated(Vec2 d) const { return Rect{ pos + d, size }; }

        bool operator==(const Rect&) const = default;
    };

    // Per-corner radii, ordered top-left, top-right, bottom-right, bottom-left — the CSS order,
    // so imported designs need no shuffling.
    struct Corners {
        f32 tl = 0.0f, tr = 0.0f, br = 0.0f, bl = 0.0f;

        constexpr Corners() = default;
        constexpr explicit Corners(f32 all) : tl(all), tr(all), br(all), bl(all) {}
        constexpr Corners(f32 tl_, f32 tr_, f32 br_, f32 bl_) : tl(tl_), tr(tr_), br(br_), bl(bl_) {}

        constexpr bool Any() const { return tl > 0.0f || tr > 0.0f || br > 0.0f || bl > 0.0f; }
        constexpr Vec4 AsVec4() const { return { tl, tr, br, bl }; }

        bool operator==(const Corners&) const = default;
    };

    // Edge insets (padding, margin, border widths).
    struct Edges {
        f32 left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;

        constexpr Edges() = default;
        constexpr explicit Edges(f32 all) : left(all), top(all), right(all), bottom(all) {}
        constexpr Edges(f32 h, f32 v) : left(h), top(v), right(h), bottom(v) {}
        constexpr Edges(f32 l, f32 t, f32 r, f32 b) : left(l), top(t), right(r), bottom(b) {}

        constexpr f32 Horizontal() const { return left + right; }
        constexpr f32 Vertical()   const { return top + bottom; }

        bool operator==(const Edges&) const = default;
    };

}
