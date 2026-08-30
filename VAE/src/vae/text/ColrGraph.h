#pragma once

#include "vae/base/Math.h"
#include "vae/vector/Path.h"

#include <functional>
#include <vector>

namespace vae::text {

    // COLR version 1: a colour glyph is a *graph of paint operations*, not a list of layers.
    //
    // Version 0 says "draw glyph 12 in palette colour 3, then glyph 13 in colour 5", and that is
    // all it can say. Version 1 says "fill the shape of glyph 12 with a radial gradient, rotated,
    // composited onto what is underneath in multiply". The difference is why this is a separate
    // file: v0 is a lookup and v1 is an interpreter, and the interpreter needs a rasterizer that
    // can fill a path through an arbitrary transform — which is `vae::vector`, and which the layer
    // reader in Font.cpp never had to reach for.
    //
    // What is read: the paint formats that fonts actually contain — layers, solid fills, linear,
    // radial and sweep gradients, every transform, glyph and colr-glyph references, and composite
    // with the Porter-Duff and separable blend modes. The variable forms (PaintVarSolid and its
    // kin) are read as their non-variable counterparts, which is exactly right at the default
    // instance and wrong only for a variation axis nothing here sets.
    class ColrGraph {
    public:
        // A colour out of CPAL, straight alpha. Parsed by the caller — CPAL is a table about
        // palettes rather than about paint, and Font.cpp already reads it for version 0.
        struct Rgba { u8 r = 0, g = 0, b = 0, a = 255; };

        // The outline of a glyph in font units, y-up, exactly as the font stores it. Supplied by
        // the caller because the outlines live behind stb_truetype in Font.cpp and this file has
        // no business knowing which rasterizer produced them.
        using Outline = std::function<bool(u32 glyph, vector::Path& path)>;

        // Straight-alpha RGBA, and where it sits relative to the pen: `left` and `top` are y-down
        // pixel offsets, matching GlyphMetrics::bearing.
        struct Picture {
            u32 width = 0, height = 0;
            std::vector<u8> pixels;      // width * height * 4, straight alpha
            i32 left = 0, top = 0;
            bool Empty() const { return width == 0 || height == 0 || pixels.empty(); }
        };

        // Null when the table has no version-1 half — which is every COLR font written before
        // 2021 and most of them since.
        static Scope<ColrGraph> Parse(const std::vector<u8>& file, u32 colr, u32 length);

        // Whether this glyph is drawn by the v1 graph. A face can carry both halves, and a glyph
        // listed in both is the v1 one: the spec says a v1-aware reader prefers it, and the two
        // are alternative drawings of the same thing rather than parts of one.
        bool Has(u32 glyph) const;

        // Whether every colour in a glyph's graph is the text colour (palette index 0xFFFF). Such
        // a glyph must NOT go down the colour path: the atlas is keyed by face, glyph and size and
        // has nowhere to put the colour the text actually asked for, so it is drawn as an outline
        // and tinted like any other letter.
        bool ForegroundOnly(u32 glyph) const;

        // The pixel box the glyph draws into, y-down from the pen. False when it draws nothing.
        bool Bounds(u32 glyph, f32 scale, const Outline& outline, Rect& out) const;

        Picture Render(u32 glyph, f32 scale, const Outline& outline,
                       const std::vector<Rgba>& palette) const;

        ColrGraph();
        ~ColrGraph();

        // The parsed table. Public because the walker that draws it is a separate type in the
        // implementation file and has to name it; there is nothing here to reach for from outside.
        struct Impl;

    private:
        Scope<Impl> m_Impl;
    };

}
