#include "Test.h"

#include "vae/base/FileSystem.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextLayout.h"

using namespace vae;
using namespace vae::text;

// Colour glyphs: a face whose glyphs are pictures rather than outlines.
//
// These need a colour emoji font installed, which every desktop has and no build of VAE ships —
// a 10 MB font is not something to vendor for a test. Without one they check that asking for an
// emoji is still safe rather than checking what it looks like.

namespace {

    Ref<Font> Emoji() {
        static Ref<Font> font = [] {
            FontDB db;
            db.ScanSystemFonts();
            return db.Resolve("Noto Color Emoji");
        }();
        return font;
    }

    constexpr u32 kGrinning = 0x1F600;

}

TEST(colour, a_colour_face_loads_even_though_it_has_no_outlines) {
    if (!Emoji()) return;                    // no emoji font on this machine
    CHECK(Emoji()->Colour());
    CHECK(Emoji()->GlyphIndex(kGrinning) != 0u);
    CHECK(Emoji()->Metrics(16.0f).LineHeight() > 0.0f);
}

TEST(colour, a_colour_glyph_rasterizes_to_rgba) {
    if (!Emoji()) return;

    const u32 glyph = Emoji()->GlyphIndex(kGrinning);
    const GlyphBitmap bitmap = Emoji()->Rasterize(glyph, 32.0f);
    CHECK(bitmap.Colour());
    CHECK_EQ(bitmap.channels, 4u);
    CHECK(bitmap.width > 0u);
    CHECK_EQ(bitmap.pixels.size(), std::size_t(bitmap.width) * bitmap.height * 4);

    // Asked for 32px and given a 136px picture, it has to come back near the size asked for —
    // the strike ships at one large size and every use of it is a downscale.
    CHECK(bitmap.width <= 64u);
    CHECK(bitmap.height <= 64u);

    // Actual colour, not a coverage mask smeared across four channels.
    bool coloured = false, opaque = false;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
        if (bitmap.pixels[i + 3] > 200) opaque = true;
        if (bitmap.pixels[i] != bitmap.pixels[i + 1] || bitmap.pixels[i + 1] != bitmap.pixels[i + 2])
            coloured = true;
    }
    CHECK(opaque);
    CHECK(coloured);
}

TEST(colour, a_colour_glyph_sits_on_the_baseline_like_a_letter) {
    if (!Emoji()) return;

    const u32 glyph = Emoji()->GlyphIndex(kGrinning);
    const GlyphMetrics metrics = Emoji()->Glyph(glyph, 32.0f);
    CHECK(!metrics.blank);
    CHECK(metrics.advance > 0.0f);
    CHECK(metrics.size.x > 0.0f);
    // Bearing is y-down from the baseline to the top of the picture, so an emoji that sits on the
    // line has a negative one — the same convention every other glyph uses.
    CHECK(metrics.bearing.y < 0.0f);
}

TEST(colour, an_emoji_in_a_sentence_falls_back_to_the_colour_face) {
    if (!Emoji()) return;

    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.ScanSystemFonts();
    db.SetDefaultFamily("JetBrains Mono Nerd Font");

    const TextStyle& style = db.Style({ "", FontWeight::Regular, FontSlant::Normal, 24.0f });
    const auto result = TextLayout::Layout("hi \xF0\x9F\x98\x80", style, 0.0f, WrapMode::None);
    CHECK_EQ(result.glyphs.size(), std::size_t(4));

    const PositionedGlyph& emoji = result.glyphs.back();
    CHECK(emoji.face != nullptr);
    if (!emoji.face) return;
    CHECK(emoji.face->Colour());
    CHECK(emoji.advance > 0.0f);
    // And it advanced the pen, so what follows it is not drawn on top of it.
    CHECK(emoji.pen.x > result.glyphs[2].pen.x);
}
