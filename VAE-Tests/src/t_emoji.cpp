#include "Test.h"

#include "FontFixture.h"

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

// --- sbix and COLR/CPAL ------------------------------------------------------------------------
//
// Neither format is installed on a Linux desktop, so these run against fonts built in the test —
// the bundled outline font with a real colour table injected into its directory. See FontFixture.h
// for why that is the shape rather than a vendored emoji font.

namespace {

    Ref<Font> LoadFixture(std::vector<u8> bytes, const char* name) {
        if (bytes.empty()) return nullptr;
        return Font::LoadFromMemory(std::move(bytes), name);
    }

    // The average colour of everything opaque in a bitmap, which is what "is this red" means for a
    // composited glyph whose edges are antialiased.
    struct Average { u32 r = 0, g = 0, b = 0, count = 0; };

    Average OpaqueAverage(const GlyphBitmap& bitmap) {
        Average out;
        for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
            if (bitmap.pixels[i + 3] < 250) continue;
            out.r += bitmap.pixels[i]; out.g += bitmap.pixels[i + 1]; out.b += bitmap.pixels[i + 2];
            ++out.count;
        }
        if (out.count) { out.r /= out.count; out.g /= out.count; out.b /= out.count; }
        return out;
    }

}

TEST(sbix, a_face_with_an_sbix_table_keeps_its_outlines_and_gains_a_picture) {
    const auto font = LoadFixture(fixture::SbixFont({ .letter = 'A' }), "sbix-fixture");
    CHECK(font != nullptr);
    if (!font) return;

    // The difference from CBDT that the whole reader turns on: this is still an outline font.
    CHECK(font->Outlines());
    CHECK(font->Colour());
    CHECK(font->ColourStorage() == ColourFormat::Sbix);

    const u32 pictured = font->GlyphIndex('A');
    const u32 plain    = font->GlyphIndex('B');
    CHECK(pictured != 0u);
    CHECK(font->HasColourGlyph(pictured));
    CHECK(!font->HasColourGlyph(plain));

    // And the glyphs without a picture still rasterize as coverage, not as nothing.
    const GlyphBitmap letter = font->Rasterize(plain, 32.0f);
    CHECK(!letter.Colour());
    CHECK_EQ(letter.channels, 1u);
    CHECK(letter.width > 0u);
}

TEST(sbix, the_picture_is_scaled_from_its_strike_to_the_size_asked_for) {
    const auto font = LoadFixture(fixture::SbixFont({ .letter = 'A', .ppem = 128, .size = 64 }),
                                  "sbix-fixture");
    if (!font) return;

    const u32 glyph = font->GlyphIndex('A');
    // 64 pixels at a 128px strike is half an em, so at 32px it is 16.
    const GlyphMetrics metrics = font->Glyph(glyph, 32.0f);
    CHECK(!metrics.blank);
    CHECK_NEAR(metrics.size.x, 16.0f);
    CHECK_NEAR(metrics.size.y, 16.0f);
    CHECK(metrics.advance > 0.0f);          // from hmtx, which an sbix face has

    const GlyphBitmap bitmap = font->Rasterize(glyph, 32.0f);
    CHECK(bitmap.Colour());
    CHECK_EQ(bitmap.width, 16u);
    CHECK_EQ(bitmap.height, 16u);

    const Average colour = OpaqueAverage(bitmap);
    CHECK(colour.count > 0u);
    CHECK(colour.r > 180u);                 // the fixture's red, not a channel swap
    CHECK(colour.g < 80u);
    CHECK(colour.b < 80u);
}

TEST(sbix, the_origin_offset_lifts_the_picture_off_the_baseline) {
    // originY is where the *bottom* goes, y-up. A 64px picture 16px above the baseline has its top
    // 80px up, which is -80 in the y-down bearing every other glyph reports — halved at 64px.
    const auto font = LoadFixture(
        fixture::SbixFont({ .letter = 'A', .ppem = 128, .size = 64, .originY = 16 }),
        "sbix-fixture");
    if (!font) return;

    const GlyphMetrics metrics = font->Glyph(font->GlyphIndex('A'), 64.0f);
    CHECK_NEAR(metrics.bearing.y, -40.0f);
    CHECK_NEAR(metrics.bearing.x, 0.0f);
}

TEST(sbix, a_dupe_entry_draws_the_glyph_it_names) {
    // One image, several characters — what `dupe` is for, and a branch that would otherwise draw
    // four bytes of tag as a picture.
    const auto font = LoadFixture(fixture::SbixFont({ .letter = 'A', .duplicate = 'Z' }),
                                  "sbix-fixture");
    if (!font) return;

    const u32 alias = font->GlyphIndex('Z');
    CHECK(font->HasColourGlyph(alias));

    const GlyphBitmap direct = font->Rasterize(font->GlyphIndex('A'), 32.0f);
    const GlyphBitmap aliased = font->Rasterize(alias, 32.0f);
    CHECK(aliased.Colour());
    CHECK_EQ(aliased.width, direct.width);
    CHECK(aliased.pixels == direct.pixels);
}

TEST(colr, a_base_glyph_is_composited_from_its_layers_in_palette_colours) {
    // 'A' is drawn as a red 'B' under a blue 'C'. Two layers, two palette entries, and the check
    // that matters is that red comes out red — CPAL stores its records BGRA.
    const auto font = LoadFixture(
        fixture::ColrFont('A', { { 'B', 0 }, { 'C', 1 } },
                          { { 255, 0, 0, 255 }, { 0, 0, 255, 255 } }),
        "colr-fixture");
    CHECK(font != nullptr);
    if (!font) return;

    CHECK(font->Outlines());
    CHECK(font->Colour());
    CHECK(font->ColourStorage() == ColourFormat::Colr);

    const u32 base = font->GlyphIndex('A');
    CHECK(font->HasColourGlyph(base));
    // Only the one glyph named by the table. Everything else in the font is an ordinary letter.
    CHECK(!font->HasColourGlyph(font->GlyphIndex('B')));

    const GlyphBitmap bitmap = font->Rasterize(base, 48.0f);
    CHECK(bitmap.Colour());
    CHECK(bitmap.width > 0u);
    CHECK_EQ(bitmap.pixels.size(), std::size_t(bitmap.width) * bitmap.height * 4);

    bool red = false, blue = false;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
        if (bitmap.pixels[i + 3] < 250) continue;
        if (bitmap.pixels[i] > 200 && bitmap.pixels[i + 2] < 60) red = true;
        if (bitmap.pixels[i + 2] > 200 && bitmap.pixels[i] < 60) blue = true;
    }
    CHECK(red);
    CHECK(blue);
}

TEST(colr, a_layered_glyph_measures_the_union_of_its_layers) {
    const auto font = LoadFixture(
        fixture::ColrFont('A', { { 'B', 0 }, { 'g', 1 } },
                          { { 255, 0, 0, 255 }, { 0, 0, 255, 255 } }),
        "colr-fixture");
    if (!font) return;

    // 'g' has a descender and 'B' does not, so the composite has to reach below the baseline —
    // the base glyph's own box would not, and a COLR base glyph is usually empty anyway.
    const GlyphMetrics layered = font->Glyph(font->GlyphIndex('A'), 48.0f);
    const GlyphMetrics plainB  = font->Glyph(font->GlyphIndex('B'), 48.0f);
    CHECK(!layered.blank);
    CHECK(layered.size.y > plainB.size.y);
    CHECK(layered.bearing.y + layered.size.y > plainB.bearing.y + plainB.size.y);
    // The advance stays the base glyph's own: layers say how it is drawn, not how wide it is.
    CHECK(layered.advance > 0.0f);
}

TEST(colr, a_glyph_drawn_only_in_the_text_colour_is_left_as_an_outline) {
    // Palette index 0xFFFF means "the colour the text is". An atlas keyed by face, glyph and size
    // has no colour to bake, so a glyph made only of those must stay on the outline path — where
    // it gets the real one — rather than being composited to black.
    const auto font = LoadFixture(
        fixture::ColrFont('A', { { 'B', 0xFFFF } }, { { 255, 0, 0, 255 } }),
        "colr-fixture");
    if (!font) return;

    const u32 base = font->GlyphIndex('A');
    CHECK(!font->HasColourGlyph(base));
    CHECK(!font->ColourCovers('A'));

    const GlyphBitmap bitmap = font->Rasterize(base, 48.0f);
    CHECK(!bitmap.Colour());
    CHECK_EQ(bitmap.channels, 1u);
}

TEST(colr, a_mixed_glyph_is_still_colour) {
    const auto font = LoadFixture(
        fixture::ColrFont('A', { { 'B', 0xFFFF }, { 'C', 0 } }, { { 0, 200, 0, 255 } }),
        "colr-fixture");
    if (!font) return;
    CHECK(font->HasColourGlyph(font->GlyphIndex('A')));

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 48.0f);
    CHECK(bitmap.Colour());
    bool green = false;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4)
        if (bitmap.pixels[i + 3] > 250 && bitmap.pixels[i + 1] > 150 && bitmap.pixels[i] < 60)
            green = true;
    CHECK(green);
}

TEST(colr, a_colour_face_still_measures_and_draws_its_ordinary_letters) {
    // The regression this guards: dispatching on the *face* rather than the glyph would send every
    // letter in a COLR font down the colour path and draw a page of nothing.
    const auto font = LoadFixture(
        fixture::ColrFont('A', { { 'B', 0 } }, { { 255, 0, 0, 255 } }), "colr-fixture");
    if (!font) return;

    const GlyphMetrics letter = font->Glyph(font->GlyphIndex('m'), 32.0f);
    CHECK(!letter.blank);
    CHECK(letter.advance > 0.0f);
    CHECK(font->Kerning(font->GlyphIndex('A'), font->GlyphIndex('V'), 32.0f) <= 0.0f);
    CHECK(font->Metrics(32.0f).LineHeight() > 0.0f);
}

TEST(colour, a_downscaled_picture_keeps_its_colour_at_the_edge) {
    // The atlas holds *straight* alpha, the same as the PNG and the same as this pipeline's blend.
    // The trap is the resample: told the data is premultiplied when it is not, it averages the raw
    // colour of fully transparent pixels — black, in every PNG encoder — into the edge, and every
    // emoji comes out with a dark halo. Only a picture with a transparent margin shows it.
    const auto font = LoadFixture(
        fixture::SbixFont({ .letter = 'A', .ppem = 128, .size = 64,
                            .red = 220, .green = 40, .blue = 40, .border = 8 }),
        "sbix-fixture");
    if (!font) return;

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 23.0f);
    CHECK(bitmap.Colour());

    bool partial = false;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
        const u8 alpha = bitmap.pixels[i + 3];
        if (alpha == 0) continue;
        if (alpha < 250) partial = true;
        // Red everywhere it is drawn at all, however faintly. Under the halo bug the edge pixels
        // are a fraction of that, because they were averaged towards black.
        CHECK(bitmap.pixels[i] > 150u);
        CHECK(bitmap.pixels[i + 1] < 110u);
        CHECK(bitmap.pixels[i + 2] < 110u);
    }
    CHECK(partial);      // an all-or-nothing bitmap would prove nothing
}
