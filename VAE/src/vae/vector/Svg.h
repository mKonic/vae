#pragma once

#include "vae/vector/Raster.h"

#include <string>
#include <string_view>

namespace vae::vector {

    // One drawable from an SVG, with the paint it was given and the transform it inherited. The
    // transform is kept rather than baked into the points so that flattening still happens at the
    // size the thing will actually be drawn — a curve baked flat at authoring size is a polygon at
    // four times the zoom.
    struct Shape {
        Path path;
        Affine transform;

        bool hasFill = true;
        Color fill{ 0.0f, 0.0f, 0.0f, 1.0f };
        FillRule rule = FillRule::NonZero;

        bool hasStroke = false;
        Color stroke{ 0.0f, 0.0f, 0.0f, 1.0f };
        f32 strokeWidth = 1.0f;
        LineCap cap = LineCap::Butt;
        LineJoin join = LineJoin::Miter;
        f32 miterLimit = 4.0f;

        f32 opacity = 1.0f;
        // `currentColor` is how an icon set says "whatever colour the surrounding text is". Kept as
        // a flag rather than resolved at parse time, because the answer belongs to the screen.
        bool fillFollowsText = false;
        bool strokeFollowsText = false;
    };

    struct Picture {
        Vec2 size{ 0.0f, 0.0f };     // what the file says it is, in its own units
        Rect viewBox{};              // the user-space box that maps onto that size
        std::vector<Shape> shapes;

        bool Empty() const { return shapes.empty(); }
        // Whether anything in it says `currentColor`. An icon set that does is asking to be told
        // what colour to be, and answering "black" is how an icon disappears on a dark screen.
        bool FollowsText() const {
            for (const Shape& shape : shapes)
                if ((shape.hasFill && shape.fillFollowsText)
                    || (shape.hasStroke && shape.strokeFollowsText)) return true;
            return false;
        }
    };

    // Reads the subset of SVG that icon sets and exported artwork actually use: paths and the
    // basic shapes, groups, transforms, presentation attributes and inline `style`. Gradients,
    // filters, clip paths, masks, `<use>` and text are skipped rather than approximated — a wrong
    // picture is worse than a missing one, and `error` says what was dropped.
    bool ParseSvg(std::string_view source, Picture& out, std::string* error = nullptr);

    // Straight-alpha RGBA8, which is what the texture format wants and what the shader samples.
    struct Bitmap {
        u32 width = 0, height = 0;
        std::vector<u8> pixels;      // width * height * 4
        bool Empty() const { return width == 0 || height == 0; }
    };

    // Draws the picture into `width` x `height` pixels, its viewBox fitted inside without being
    // stretched. A `tint` replaces every colour in the file, which is how an icon obeys a theme
    // token instead of the palette its author happened to choose.
    Bitmap Render(const Picture& picture, u32 width, u32 height, const Color* tint = nullptr);

}
