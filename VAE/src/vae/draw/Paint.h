#pragma once

#include "vae/base/Math.h"
#include "vae/gpu/Resources.h"

namespace vae::draw {

    // How a shape is filled. Gradient coordinates are normalized to the shape's own box (0,0 =
    // top-left, 1,1 = bottom-right) so a fill survives the shape being moved or resized — which is
    // what a designer expects when they drag a frame around.
    struct Paint {
        // Glyph is a fill kind rather than a separate pipeline, so text batches together with
        // the boxes around it instead of forcing a pipeline switch per label.
        enum class Kind : u32 { Solid = 0, LinearGradient = 1, RadialGradient = 2, Image = 3,
                                Glyph = 4 };

        Kind  kind = Kind::Solid;
        Color color{ 1.0f, 1.0f, 1.0f, 1.0f };    // solid fill, gradient stop 0, or image tint
        Color color1{ 1.0f, 1.0f, 1.0f, 1.0f };   // gradient stop 1
        Vec2  from{ 0.0f, 0.0f };                 // linear: start; radial: centre
        Vec2  to{ 0.0f, 1.0f };                   // linear: end;   radial: (radius, unused)
        Ref<gpu::Texture> image;
        Rect  uv{ { 0.0f, 0.0f }, { 1.0f, 1.0f } };

        static Paint Solid(Color c) { Paint p; p.kind = Kind::Solid; p.color = c; return p; }

        static Paint Linear(Color a, Color b, Vec2 from = { 0.0f, 0.0f }, Vec2 to = { 0.0f, 1.0f }) {
            Paint p; p.kind = Kind::LinearGradient;
            p.color = a; p.color1 = b; p.from = from; p.to = to;
            return p;
        }

        static Paint Radial(Color inner, Color outer,
                            Vec2 center = { 0.5f, 0.5f }, f32 radius = 0.5f) {
            Paint p; p.kind = Kind::RadialGradient;
            p.color = inner; p.color1 = outer; p.from = center; p.to = { radius, 0.0f };
            return p;
        }

        static Paint Image(Ref<gpu::Texture> tex, Color tint = { 1.0f, 1.0f, 1.0f, 1.0f }) {
            Paint p; p.kind = Kind::Image; p.image = std::move(tex); p.color = tint;
            return p;
        }
    };

    struct Stroke {
        f32   width = 0.0f;                        // 0 = no border
        Color color{ 0.0f, 0.0f, 0.0f, 1.0f };
    };

    // Blur is the visual blur radius, matching how designers think about it (and how CSS and Figma
    // spell it); the shader converts it to a Gaussian sigma.
    struct ShadowSpec {
        Color color{ 0.0f, 0.0f, 0.0f, 0.35f };
        Vec2  offset{ 0.0f, 4.0f };
        f32   blur = 12.0f;
        f32   spread = 0.0f;
    };

}
