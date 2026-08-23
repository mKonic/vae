#include "vaepch.h"
#include "vae/text/TextDraw.h"

#include "vae/text/TextCache.h"

#include <cmath>

namespace vae::text {

    void DrawGlyphs(draw::DrawList& list, GlyphAtlas& atlas, const TextLayoutResult& layout,
                    Vec2 origin, Color color, f32 pixelSize, f32 ratio) {
        if (ratio <= 0.0f) ratio = 1.0f;
        const f32 inverse = 1.0f / ratio;

        for (const auto& glyph : layout.glyphs) {
            if (!glyph.face) continue;

            // Rasterized at device resolution, placed and sized in logical units.
            const GlyphAtlas::Entry* entry = atlas.Get(*glyph.face, glyph.glyph, pixelSize * ratio);
            if (!entry || entry->blank) continue;

            // Snapped on the device grid, not the logical one. Text at a fractional device offset
            // resamples through the atlas and goes soft, which is the single most visible quality
            // difference in small UI type — and at 1.5x scaling every logical half-pixel is one.
            // The shaper's offset is part of the position, not of the pen: it is how a mark lands
            // on the letter it belongs to, so it is scaled with everything else and snapped once.
            const Vec2 pen{ glyph.pen.x + glyph.offset.x, glyph.pen.y + glyph.offset.y };
            const Vec2 device{ std::round((origin.x + pen.x) * ratio + entry->bearing.x),
                               std::round((origin.y + pen.y) * ratio + entry->bearing.y) };

            list.AddGlyph(Rect{ device * inverse, entry->size * inverse }, entry->uv, color,
                          atlas.PageTexture(entry->page));
        }
    }

    Vec2 DrawText(draw::DrawList& list, GlyphAtlas& atlas, std::string_view utf8,
                  const TextStyle& style, Vec2 origin, Color color,
                  f32 maxWidth, WrapMode wrap, TextAlign align, f32 ratio) {
        const TextLayoutResult& layout = TextCache::Layout(utf8, style, maxWidth, wrap, align);
        DrawGlyphs(list, atlas, layout, origin, color, style.size, ratio);
        return layout.size;
    }

}
