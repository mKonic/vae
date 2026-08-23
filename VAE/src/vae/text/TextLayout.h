#pragma once

#include "vae/text/Font.h"

namespace vae::text {

    enum class TextAlign : u8 { Left, Center, Right };
    enum class WrapMode  : u8 { None, Word, Char };

    class FontDB;
    enum class FontWeight : u16;
    enum class FontSlant : u8;

    struct TextStyle {
        Ref<Font> font;
        // Tried in order when `font` has no glyph for a codepoint. Without this, a Nerd Font icon
        // or a CJK character in an otherwise Latin string draws as tofu; FontDB fills this in.
        std::vector<Ref<Font>> fallbacks;
        f32  size = 14.0f;
        f32  lineHeight = 0.0f;      // 0 = the font's own line height
        f32  letterSpacing = 0.0f;
        bool kerning = true;

        // Who built this chain, and what was asked of it. Set by FontDB::Style; a hand-made style
        // leaves it null and simply has no last resort beyond `fallbacks`.
        FontDB* db = nullptr;
        FontWeight weight{};
        FontSlant  slant{};

        // The face that actually owns this codepoint, or `font` if nothing does. Beyond `font` and
        // `fallbacks` this asks the database for any installed face that covers the character, so
        // a script nobody listed still draws rather than coming out as boxes.
        const Ref<Font>& FaceFor(u32 codepoint) const;
    };

    struct PositionedGlyph {
        u32  glyph = 0;              // glyph index in `face` — what the atlas and the shaper speak
        u32  codepoint = 0;          // first codepoint of the cluster, for breaking and hit-testing
        Font* face = nullptr;        // which face resolved it, after fallback
        Vec2 pen{ 0.0f, 0.0f };      // x = left edge, y = baseline
        Vec2 offset{ 0.0f, 0.0f };   // shaper's positioning delta, for attached marks
        f32  advance = 0.0f;
        u32  line = 0;
        std::size_t byteOffset = 0;  // into the source string, for hit-testing and carets
        bool cluster = true;         // starts a new cluster: a caret may sit before it
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

    // Shaping, line breaking and alignment.
    //
    // Text is split into runs at font-fallback, script and direction boundaries, each run is shaped
    // by HarfBuzz, and the shaped glyphs are then broken into lines. That ordering is what makes
    // Arabic joining, Devanagari reordering and ligature substitution come out right: shaping has
    // to see a whole word, so it cannot be done one codepoint at a time while measuring.
    //
    // Bidirectional text is handled at two levels — a paragraph direction taken from its first
    // strong character, with opposite-direction runs reordered inside it. That is correct for the
    // strings a UI actually holds ("مرحبا", "Hello عالم") and not the full UAX #9 algorithm, which
    // resolves arbitrarily nested embeddings and overrides.
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
