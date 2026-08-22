#pragma once

#include "vae/draw/DrawList.h"
#include "vae/text/GlyphAtlas.h"

namespace vae::text {

    // Emits one glyph quad per visible glyph into the draw list. Glyph quads go through the same
    // instanced quad pipeline as everything else — they are a fill kind, not a separate renderer —
    // so text batches together with the boxes around it.
    // `ratio` is device pixels per logical pixel. Glyphs are rasterized at the physical size and
    // the quads are emitted at logical size, so a HiDPI display gets real detail rather than a
    // magnified 96dpi bitmap — which is the difference between crisp type and the type everyone
    // recognises as "an app that does not support HiDPI".
    void DrawGlyphs(draw::DrawList& list, GlyphAtlas& atlas, const TextLayoutResult& layout,
                    Vec2 origin, Color color, f32 pixelSize, f32 ratio = 1.0f);

    // Convenience: lay out and draw in one call. Returns the laid-out box.
    Vec2 DrawText(draw::DrawList& list, GlyphAtlas& atlas, std::string_view utf8,
                  const TextStyle& style, Vec2 origin, Color color,
                  f32 maxWidth = 0.0f, WrapMode wrap = WrapMode::Word,
                  TextAlign align = TextAlign::Left, f32 ratio = 1.0f);

}
