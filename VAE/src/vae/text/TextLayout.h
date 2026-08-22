#pragma once

#include "vae/text/Font.h"

namespace vae::text {

    enum class TextAlign : u8 { Left, Center, Right };
    enum class WrapMode  : u8 { None, Word, Char };

    struct TextStyle {
        Ref<Font> font;
        // Tried in order when `font` has no glyph for a codepoint. Without this, a Nerd Font icon
        // or a CJK character in an otherwise Latin string draws as tofu; FontDB fills this in.
        std::vector<Ref<Font>> fallbacks;
        f32  size = 14.0f;
        f32  lineHeight = 0.0f;      // 0 = the font's own line height
        f32  letterSpacing = 0.0f;
        bool kerning = true;

        // The face that actually owns this codepoint, or `font` if nothing does.
        const Ref<Font>& FaceFor(u32 codepoint) const;
    };

    struct PositionedGlyph {
        u32  codepoint = 0;
        Font* face = nullptr;        // which face resolved it, after fallback
        Vec2 pen{ 0.0f, 0.0f };      // x = left edge, y = baseline
        f32  advance = 0.0f;
        u32  line = 0;
        std::size_t byteOffset = 0;  // into the source string, for hit-testing and carets
    };

    struct TextLine {
        u32 firstGlyph = 0;
        u32 glyphCount = 0;
        f32 width = 0.0f;            // excludes trailing whitespace
        f32 baselineY = 0.0f;
    };

    struct TextLayoutResult {
        std::vector<PositionedGlyph> glyphs;
        std::vector<TextLine> lines;
        Vec2 size{ 0.0f, 0.0f };     // bounding box: widest line by total line height
    };

    // Shaping seam. The simple shaper does codepoint-to-glyph mapping with kerning, which covers
    // Latin, Greek and Cyrillic. Scripts needing reordering, ligature substitution or mark
    // attachment (Arabic, Devanagari, emoji sequences) need HarfBuzz behind this same interface —
    // an accepted gap, not an oversight.
    class TextLayout {
    public:
        // maxWidth 0 means unbounded.
        static TextLayoutResult Layout(std::string_view utf8, const TextStyle& style,
                                       f32 maxWidth = 0.0f,
                                       WrapMode wrap = WrapMode::Word,
                                       TextAlign align = TextAlign::Left);

        // Cheap path for layout, which only needs the box.
        static Vec2 Measure(std::string_view utf8, const TextStyle& style, f32 maxWidth = 0.0f,
                            WrapMode wrap = WrapMode::Word);

        // Byte offset of the caret nearest to x on the given line.
        static std::size_t HitTest(const TextLayoutResult& result, Vec2 point);
    };

}
