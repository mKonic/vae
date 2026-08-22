#pragma once

#include "vae/vector/Path.h"

namespace vae::vector {

    // Single-channel coverage, the same thing the glyph atlas holds — so a filled path can be
    // painted by the pipeline that already exists rather than by a second one that would have to
    // agree with it about antialiasing.
    struct Mask {
        u32 width = 0, height = 0;
        std::vector<u8> coverage;

        bool Empty() const { return width == 0 || height == 0; }
        u8 At(u32 x, u32 y) const {
            return x < width && y < height ? coverage[static_cast<std::size_t>(y) * width + x] : 0;
        }
    };

    enum class LineCap  : u8 { Butt, Round, Square };
    enum class LineJoin : u8 { Miter, Round, Bevel };

    // Fills the contours into a `width` x `height` mask whose top-left is the origin of the space
    // the contours are already in. Open contours are treated as closed, which is what filling one
    // means.
    Mask Fill(const std::vector<Contour>& contours, FillRule rule, u32 width, u32 height);

    // The outline of a stroke, as contours that mean the same stroke when filled with the nonzero
    // rule. Every piece — the segment bodies, the joins, the caps — is wound the same way, so
    // overlapping them adds up instead of cancelling out into holes.
    std::vector<Contour> Stroke(const std::vector<Contour>& contours, f32 width,
                                LineJoin join = LineJoin::Miter, LineCap cap = LineCap::Butt,
                                f32 miterLimit = 4.0f);

}
