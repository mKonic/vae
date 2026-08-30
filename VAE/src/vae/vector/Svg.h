#pragma once

#include "vae/vector/Raster.h"

#include <string>
#include <string_view>

namespace vae::vector {

    // One drawable from an SVG, with the paint it was given and the transform it inherited. The
    // transform is kept rather than baked into the points so that flattening still happens at the
    // size the thing will actually be drawn — a curve baked flat at authoring size is a polygon at
    // four times the zoom.
    // A gradient the file defined and a shape referred to by name. Kept on the picture rather than
    // copied into every shape that uses one, because that is how the file says it: one definition,
    // any number of references, and a `gradientUnits` that means "of whatever is being filled".
    struct GradientStop {
        f32 offset = 0.0f;
        Color colour{ 0.0f, 0.0f, 0.0f, 1.0f };
    };

    struct Gradient {
        enum class Kind : u8 { Linear, Radial };
        enum class Spread : u8 { Pad, Repeat, Reflect };

        Kind kind = Kind::Linear;
        Spread spread = Spread::Pad;
        // Linear: the line from `from` to `to`. Radial: a circle at `centre` of radius `radius`,
        // with the gradient starting at `focus` — which is what makes a highlight look lit.
        Vec2 from{ 0.0f, 0.0f }, to{ 1.0f, 0.0f };
        Vec2 centre{ 0.5f, 0.5f }, focus{ 0.5f, 0.5f };
        f32  radius = 0.5f;
        // Whether the numbers above are user-space coordinates or fractions of the box of whatever
        // is being filled. The second is SVG's default and the reason a gradient has to be resolved
        // against a shape rather than baked at parse time.
        bool userSpace = false;
        Affine transform;
        std::vector<GradientStop> stops;
    };

    struct Shape {
        Path path;
        Affine transform;

        bool hasFill = true;
        Color fill{ 0.0f, 0.0f, 0.0f, 1.0f };
        FillRule rule = FillRule::NonZero;
        // Which of the picture's gradients paints this, or -1 for the flat colour above.
        i32 fillGradient = -1;
        i32 strokeGradient = -1;

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
        std::vector<Gradient> gradients;

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
    // basic shapes, groups, transforms, presentation attributes and inline `style`, and linear and
    // radial gradients. Filters, clip paths, masks, `<use>` and text are skipped rather than
    // approximated — a wrong picture is worse than a missing one, and `error` says what was
    // dropped.
    //
    // `onlyId` keeps just the subtree of the element with that id, which is how an OpenType-SVG
    // font asks for one glyph out of a document that draws several. Definitions outside it are
    // still read, because that is where the gradients live. An id that is not there keeps
    // everything, so a single-glyph document works without knowing which shape it is.
    bool ParseSvg(std::string_view source, Picture& out, std::string* error = nullptr,
                  std::string_view onlyId = {});

    // The box the picture actually draws in, in its own user units. Not the viewBox: a glyph
    // document has no viewBox worth believing and something still has to decide how big a bitmap
    // to make.
    Rect PictureBounds(const Picture& picture);

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

    // The same, with the caller deciding where the picture goes rather than fitting its viewBox.
    // A font glyph is the case that needs it: its document is in font units with the pen at the
    // origin, and how it maps onto pixels is the text's business, not the file's.
    Bitmap RenderTransformed(const Picture& picture, u32 width, u32 height, const Affine& toPixels,
                             const Color* tint = nullptr);

}
