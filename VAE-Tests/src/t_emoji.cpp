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

TEST(colour, a_truncated_or_corrupt_colour_table_is_refused_rather_than_read_past) {
    // A font is a file from somewhere else, and these readers walk offsets out of it. Every one of
    // them is a chance to read past the end. Loading a hundred broken versions of a good font is
    // worth more than any amount of staring at the bounds checks — and under the sanitized build
    // (premake5 gmake --sanitize=address,undefined) it is what proves them.
    const std::vector<fixture::PaletteEntry> palette{ { 255, 0, 0, 255 }, { 0, 0, 255, 255 } };
    for (std::vector<u8> whole : { fixture::SbixFont({ .letter = 'A' }),
                                   fixture::ColrFont('A', { { 'B', 0 }, { 'C', 1 } }, palette),
                                   // And the paint graph, which is the reader with the most
                                   // offsets in it by far: every paint names its children by a
                                   // delta from itself, and a corrupt one is a jump to anywhere.
                                   fixture::ColrV1Font({ .base = 'A', .shape = 'B', .under = 'C',
                                                         .fill = fixture::ColrV1Spec::Fill::Radial,
                                                         .transform =
                                                             fixture::ColrV1Spec::Transform::Rotate,
                                                         .amount = 30.0f,
                                                         .viaLayers = true, .composite = 3 },
                                                       palette),
                                   // And the one whose contents are a file: a corrupt gzip stream
                                   // or a truncated document is XML that stops mid-tag, and the
                                   // parser is fed it either way.
                                   fixture::SvgFont({ .letter = 'A', .second = 'B',
                                                      .gzip = true }) }) {
        if (whole.empty()) continue;

        for (std::size_t cut = 1; cut < 128; ++cut) {
            // Cut at a spread of lengths rather than every one: the interesting places are the
            // table directory, the table headers and the middle of an offset array.
            const std::size_t keep = whole.size() * cut / 128;
            std::vector<u8> truncated(whole.begin(), whole.begin() + static_cast<long>(keep));
            const auto font = Font::LoadFromMemory(std::move(truncated), "truncated");
            // Some prefixes are still a readable outline font and some are nothing. Either answer
            // is fine; reading past the end of the buffer is not, and that is what is under test.
            if (font) {
                const u32 glyph = font->GlyphIndex('A');
                font->Glyph(glyph, 24.0f);
                font->Rasterize(glyph, 24.0f);
                font->HasColourGlyph(glyph);
            }
        }

        // And bytes flipped in place, which truncation never produces: a length that is too big,
        // an offset that points backwards, a count that overruns its own table.
        for (std::size_t at = 0; at < whole.size(); at += 397) {
            std::vector<u8> corrupt = whole;
            corrupt[at] = static_cast<u8>(~corrupt[at]);
            const auto font = Font::LoadFromMemory(std::move(corrupt), "corrupt");
            if (font) {
                const u32 glyph = font->GlyphIndex('A');
                font->Glyph(glyph, 24.0f);
                font->Rasterize(glyph, 24.0f);
            }
        }
    }
    CHECK(true);        // reaching here without the sanitizer stopping us is the assertion
}

// --- COLR version 1: a paint graph rather than a layer list -------------------------------------
//
// Version 0 can say "this glyph, in that colour" and nothing else. Version 1 is an interpreter:
// gradients, transforms, nested references and compositing. Every one of these builds a real table
// and reads it back through the same path a font from the internet would take.

namespace {

    // The palette both halves of these tests use: red, blue, green.
    const std::vector<fixture::PaletteEntry> kPalette{
        { 255, 0, 0, 255 }, { 0, 0, 255, 255 }, { 0, 200, 0, 255 },
    };

    Ref<Font> V1(const fixture::ColrV1Spec& spec) {
        return LoadFixture(fixture::ColrV1Font(spec, kPalette), "colrv1-fixture");
    }

    // Does anything opaque in the bitmap look like this colour?
    bool HasColour(const GlyphBitmap& bitmap, u8 r, u8 g, u8 b, u32 slack = 40) {
        const auto near = [&](u8 value, u8 want) {
            return value >= (want > slack ? want - slack : 0u)
                && value <= std::min<u32>(255u, want + slack);
        };
        for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4)
            if (bitmap.pixels[i + 3] > 250 && near(bitmap.pixels[i], r)
                && near(bitmap.pixels[i + 1], g) && near(bitmap.pixels[i + 2], b))
                return true;
        return false;
    }

}

TEST(colrv1, a_paint_graph_draws_a_solid_fill_clipped_to_a_glyph) {
    const auto font = V1({ .base = 'A', .shape = 'B', .palette = 0 });
    CHECK(font != nullptr);
    if (!font) return;

    CHECK(font->Outlines());
    CHECK(font->ColourStorage() == ColourFormat::Colr);

    const u32 base = font->GlyphIndex('A');
    CHECK(font->HasColourGlyph(base));
    // The glyph the paint fills is not itself a colour glyph — it is a shape being borrowed.
    CHECK(!font->HasColourGlyph(font->GlyphIndex('B')));

    const GlyphBitmap bitmap = font->Rasterize(base, 48.0f);
    CHECK(bitmap.Colour());
    CHECK(bitmap.width > 0u);
    CHECK_EQ(bitmap.pixels.size(), std::size_t(bitmap.width) * bitmap.height * 4);
    CHECK(HasColour(bitmap, 255, 0, 0));

    // And it is the shape of a 'B': the fill is clipped to the outline, not to the box.
    const Average average = OpaqueAverage(bitmap);
    CHECK(average.count > 0u);
    CHECK(average.count < static_cast<u32>(bitmap.width) * bitmap.height);
}

TEST(colrv1, a_linear_gradient_runs_from_one_stop_to_the_other) {
    const auto font = V1({ .base = 'A', .shape = 'B',
                           .fill = fixture::ColrV1Spec::Fill::Linear,
                           .palette = 0, .palette2 = 1 });
    if (!font) return;

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 64.0f);
    CHECK(bitmap.Colour());
    // Red at the left end, blue at the right, and something in between in between — which is the
    // difference between a gradient and two flat halves.
    CHECK(HasColour(bitmap, 255, 0, 0, 60));
    CHECK(HasColour(bitmap, 0, 0, 255, 60));
    bool mixed = false;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4)
        if (bitmap.pixels[i + 3] > 250 && bitmap.pixels[i] > 60 && bitmap.pixels[i] < 195
            && bitmap.pixels[i + 2] > 60 && bitmap.pixels[i + 2] < 195)
            mixed = true;
    CHECK(mixed);

    // Left of centre is redder than right of centre: the gradient runs the way it was authored.
    u32 leftRed = 0, rightRed = 0, leftCount = 0, rightCount = 0;
    for (u32 y = 0; y < bitmap.height; ++y)
        for (u32 x = 0; x < bitmap.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
            if (bitmap.pixels[i + 3] < 250) continue;
            if (x < bitmap.width / 3)          { leftRed  += bitmap.pixels[i]; ++leftCount; }
            else if (x > bitmap.width * 2 / 3) { rightRed += bitmap.pixels[i]; ++rightCount; }
        }
    CHECK(leftCount > 0u);
    CHECK(rightCount > 0u);
    if (leftCount && rightCount) CHECK(leftRed / leftCount > rightRed / rightCount);
}

TEST(colrv1, a_radial_gradient_is_a_circle_and_not_a_stripe) {
    const auto font = V1({ .base = 'A', .shape = 'B',
                           .fill = fixture::ColrV1Spec::Fill::Radial,
                           .palette = 0, .palette2 = 1 });
    if (!font) return;

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 64.0f);
    CHECK(bitmap.Colour());
    CHECK(HasColour(bitmap, 255, 0, 0, 70));
    CHECK(HasColour(bitmap, 0, 0, 255, 70));

    // The middle is the inner stop and the corners are the outer one. A gradient that came out as
    // a stripe would have the same colour all the way down the centre column.
    const auto redAt = [&](u32 x, u32 y) -> i32 {
        if (x >= bitmap.width || y >= bitmap.height) return -1;
        const std::size_t i = (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
        return bitmap.pixels[i + 3] > 250 ? static_cast<i32>(bitmap.pixels[i]) : -1;
    };
    const i32 centre = redAt(bitmap.width / 2, bitmap.height / 2);
    const i32 top    = redAt(bitmap.width / 2, 2);
    if (centre >= 0 && top >= 0) CHECK(centre > top);
}

TEST(colrv1, a_sweep_gradient_turns_around_its_centre) {
    const auto font = V1({ .base = 'A', .shape = 'B',
                           .fill = fixture::ColrV1Spec::Fill::Sweep,
                           .palette = 0, .palette2 = 1 });
    if (!font) return;

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 64.0f);
    CHECK(bitmap.Colour());
    // Above and below the centre are at opposite ends of the sweep, so they cannot match.
    const auto sample = [&](u32 x, u32 y) -> i32 {
        const std::size_t i = (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
        return bitmap.pixels[i + 3] > 250 ? static_cast<i32>(bitmap.pixels[i]) : -1;
    };
    const i32 above = sample(bitmap.width / 2, bitmap.height / 4);
    const i32 below = sample(bitmap.width / 2, bitmap.height * 3 / 4);
    if (above >= 0 && below >= 0) CHECK(std::abs(above - below) > 20);
}

TEST(colrv1, a_transform_moves_what_it_wraps) {
    const auto plain = V1({ .base = 'A', .shape = 'B', .palette = 0 });
    const auto moved = V1({ .base = 'A', .shape = 'B', .palette = 0,
                            .transform = fixture::ColrV1Spec::Transform::Translate,
                            .amount = 300.0f });
    if (!plain || !moved) return;

    const GlyphMetrics before = plain->Glyph(plain->GlyphIndex('A'), 48.0f);
    const GlyphMetrics after  = moved->Glyph(moved->GlyphIndex('A'), 48.0f);
    CHECK(!before.blank);
    CHECK(!after.blank);
    // Right and up in font space, which is right and *up* on screen — y is down here, so the
    // bearing decreases.
    CHECK(after.bearing.x > before.bearing.x);
    CHECK(after.bearing.y < before.bearing.y);
}

TEST(colrv1, a_scale_makes_the_glyph_bigger_and_a_matrix_agrees_with_it) {
    const auto plain  = V1({ .base = 'A', .shape = 'B', .palette = 0 });
    const auto scaled = V1({ .base = 'A', .shape = 'B', .palette = 0,
                             .transform = fixture::ColrV1Spec::Transform::Scale, .amount = 1.5f });
    const auto matrix = V1({ .base = 'A', .shape = 'B', .palette = 0,
                             .transform = fixture::ColrV1Spec::Transform::Matrix, .amount = 1.5f });
    if (!plain || !scaled || !matrix) return;

    const GlyphMetrics one = plain->Glyph(plain->GlyphIndex('A'), 48.0f);
    const GlyphMetrics big = scaled->Glyph(scaled->GlyphIndex('A'), 48.0f);
    const GlyphMetrics general = matrix->Glyph(matrix->GlyphIndex('A'), 48.0f);
    CHECK(big.size.x > one.size.x);
    CHECK(big.size.y > one.size.y);
    // PaintScale and the general Affine2x3 saying the same thing have to *mean* the same thing:
    // the matrix is 16.16 fixed point and the scale is 2.14, and reading either at the other's
    // radix is a glyph 16 384 times too big.
    CHECK(std::abs(general.size.x - big.size.x) <= 1.0f);
    CHECK(std::abs(general.size.y - big.size.y) <= 1.0f);
}

TEST(colrv1, a_rotation_turns_the_glyph) {
    const auto plain   = V1({ .base = 'A', .shape = 'L', .palette = 0 });
    const auto rotated = V1({ .base = 'A', .shape = 'L', .palette = 0,
                              .transform = fixture::ColrV1Spec::Transform::Rotate,
                              .amount = 90.0f });
    if (!plain || !rotated) return;

    const GlyphMetrics before = plain->Glyph(plain->GlyphIndex('A'), 64.0f);
    const GlyphMetrics after  = rotated->Glyph(rotated->GlyphIndex('A'), 64.0f);
    CHECK(!after.blank);
    // A letter is taller than it is wide; turned a quarter turn it is wider than it is tall.
    CHECK(before.size.y > before.size.x);
    CHECK(after.size.x > after.size.y);
}

TEST(colrv1, layers_are_painted_in_the_order_the_list_gives_them) {
    // 'B' in red over 'C' in blue, reached through PaintColrLayers and the layer list — the one
    // paint whose children live in a table of their own, addressed by index.
    const auto font = V1({ .base = 'A', .shape = 'B', .under = 'C', .palette = 0,
                           .underPalette = 1, .viaLayers = true });
    if (!font) return;

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 48.0f);
    CHECK(bitmap.Colour());
    CHECK(HasColour(bitmap, 255, 0, 0));
    CHECK(HasColour(bitmap, 0, 0, 255));
}

TEST(colrv1, compositing_in_source_in_keeps_only_the_overlap) {
    // The mode that cannot be faked by painting one thing after another: source-in shows the
    // source only where the backdrop already was.
    const auto over = V1({ .base = 'A', .shape = 'B', .under = 'C', .palette = 0,
                           .underPalette = 1, .composite = 3 });          // src-over
    const auto in   = V1({ .base = 'A', .shape = 'B', .under = 'C', .palette = 0,
                           .underPalette = 1, .composite = 5 });          // src-in
    if (!over || !in) return;

    const GlyphBitmap drawnOver = over->Rasterize(over->GlyphIndex('A'), 48.0f);
    const GlyphBitmap drawnIn   = in->Rasterize(in->GlyphIndex('A'), 48.0f);
    CHECK(drawnOver.Colour());
    CHECK(drawnIn.Colour());

    const auto opaque = [](const GlyphBitmap& bitmap) {
        u32 count = 0;
        for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4)
            if (bitmap.pixels[i + 3] > 250) ++count;
        return count;
    };
    CHECK(opaque(drawnOver) > opaque(drawnIn));
    // Source-in keeps no backdrop at all: nothing blue survives.
    CHECK(!HasColour(drawnIn, 0, 0, 255, 20));
    CHECK(HasColour(drawnOver, 0, 0, 255, 20));
}

TEST(colrv1, a_clip_box_is_believed_when_the_font_ships_one) {
    // The list exists so a reader does not have to walk the graph to size a glyph. A font that
    // ships one has measured itself, and a glyph is allowed to draw less than its box.
    const auto measured = V1({ .base = 'A', .shape = 'B', .palette = 0 });
    const auto boxed    = V1({ .base = 'A', .shape = 'B', .palette = 0,
                               .clipXMin = 0, .clipYMin = 0, .clipXMax = 2000, .clipYMax = 2000 });
    if (!measured || !boxed) return;

    const GlyphMetrics tight = measured->Glyph(measured->GlyphIndex('A'), 32.0f);
    const GlyphMetrics wide  = boxed->Glyph(boxed->GlyphIndex('A'), 32.0f);
    CHECK(wide.size.x > tight.size.x);
    CHECK(wide.size.y > tight.size.y);
}

TEST(colrv1, a_graph_painted_only_in_the_text_colour_stays_an_outline) {
    const auto font = V1({ .base = 'A', .shape = 'B', .palette = 0xFFFF });
    if (!font) return;

    const u32 base = font->GlyphIndex('A');
    CHECK(!font->HasColourGlyph(base));
    CHECK(!font->ColourCovers('A'));
    const GlyphBitmap bitmap = font->Rasterize(base, 48.0f);
    CHECK_EQ(bitmap.channels, 1u);
}

TEST(colrv1, the_rest_of_the_font_is_still_an_ordinary_font) {
    const auto font = V1({ .base = 'A', .shape = 'B', .palette = 0 });
    if (!font) return;

    const GlyphMetrics letter = font->Glyph(font->GlyphIndex('M'), 24.0f);
    CHECK(!letter.blank);
    CHECK(letter.advance > 0.0f);
    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('M'), 24.0f);
    CHECK_EQ(bitmap.channels, 1u);
    CHECK(bitmap.width > 0u);
}

// --- OpenType-SVG: the glyph is a document ------------------------------------------------------
//
// The fourth colour format, and the only one whose contents are a file in another language. What
// these check is the seam: the table says which document draws which glyph, the document is often
// gzipped, and the drawing inside it is in font units with the y axis pointing down — which is the
// half of the format a reader gets upside down and still produces a picture.

namespace {

    Ref<Font> Svg(const fixture::SvgSpec& spec) {
        return LoadFixture(fixture::SvgFont(spec), "svg-fixture");
    }

    // The bundled font is a 1000-unit em, so a rectangle in font units is that many thousandths
    // of the pixel size. Written out because it is the whole coordinate system under test.
    f32 Units(i32 fontUnits, f32 pixelSize) {
        return static_cast<f32>(fontUnits) * pixelSize / 1000.0f;
    }

}

TEST(otsvg, a_face_with_an_svg_table_draws_its_glyph_from_a_document) {
    const auto font = Svg({ .letter = 'A' });
    CHECK(font != nullptr);
    if (!font) return;

    CHECK(font->Outlines());                 // the format sits on top of an ordinary font
    CHECK(font->Colour());
    CHECK(font->ColourStorage() == ColourFormat::Svg);

    const u32 glyph = font->GlyphIndex('A');
    CHECK(font->HasColourGlyph(glyph));
    CHECK(font->ColourCovers('A'));
    CHECK(!font->HasColourGlyph(font->GlyphIndex('B')));

    const GlyphBitmap bitmap = font->Rasterize(glyph, 50.0f);
    CHECK(bitmap.Colour());
    CHECK_EQ(bitmap.pixels.size(), std::size_t(bitmap.width) * bitmap.height * 4);
    CHECK(HasColour(bitmap, 220, 40, 40));
}

TEST(otsvg, the_document_is_in_font_units_with_the_y_axis_pointing_down) {
    // The rectangle is 600 units wide from x=100, and 600 tall from y=-700 — that is 700 units
    // *above* the baseline, because down is positive. A reader that flipped the axis puts it
    // below the line and still draws a red box, which is why the bearing is checked and not just
    // the size.
    const auto font = Svg({ .letter = 'A', .x = 100, .y = -700, .width = 600, .height = 600 });
    if (!font) return;

    const GlyphMetrics metrics = font->Glyph(font->GlyphIndex('A'), 50.0f);
    CHECK(!metrics.blank);
    CHECK_NEAR(metrics.size.x, Units(600, 50.0f));
    CHECK_NEAR(metrics.size.y, Units(600, 50.0f));
    CHECK_NEAR(metrics.bearing.x, Units(100, 50.0f));
    CHECK_NEAR(metrics.bearing.y, Units(-700, 50.0f));
    CHECK(metrics.advance > 0.0f);           // from hmtx, not from the drawing

    const GlyphBitmap bitmap = font->Rasterize(font->GlyphIndex('A'), 50.0f);
    CHECK_EQ(bitmap.width, 30u);
    CHECK_EQ(bitmap.height, 30u);
}

TEST(otsvg, a_gzipped_document_reads_the_same_as_a_plain_one) {
    // Which is the point: the table does not say which it is, so the reader decides from the
    // magic number and has to step over a gzip header that is not part of the deflate stream.
    const auto plain  = Svg({ .letter = 'A' });
    const auto zipped = Svg({ .letter = 'A', .gzip = true });
    CHECK(zipped != nullptr);
    if (!plain || !zipped) return;

    CHECK(zipped->ColourStorage() == ColourFormat::Svg);
    const GlyphBitmap a = plain->Rasterize(plain->GlyphIndex('A'), 40.0f);
    const GlyphBitmap b = zipped->Rasterize(zipped->GlyphIndex('A'), 40.0f);
    CHECK(b.width > 0u);
    CHECK_EQ(a.width, b.width);
    CHECK_EQ(a.height, b.height);
    CHECK(a.pixels == b.pixels);
}

TEST(otsvg, one_document_can_draw_more_than_one_glyph) {
    // The usual shape of a real font: a handful of documents, each holding many glyphs, found by
    // an id of "glyphNNN". Drawing the whole document for every glyph in it is the failure this
    // catches — both glyphs would then carry both colours.
    const auto font = Svg({ .letter = 'A', .second = 'B', .gzip = true });
    if (!font) return;

    const GlyphBitmap first  = font->Rasterize(font->GlyphIndex('A'), 40.0f);
    const GlyphBitmap second = font->Rasterize(font->GlyphIndex('B'), 40.0f);
    CHECK(first.Colour());
    CHECK(second.Colour());
    CHECK(HasColour(first, 220, 40, 40));
    CHECK(!HasColour(first, 40, 80, 220));
    CHECK(HasColour(second, 40, 80, 220));
    CHECK(!HasColour(second, 220, 40, 40));
}

TEST(otsvg, each_glyph_may_instead_have_a_document_of_its_own) {
    const auto font = Svg({ .letter = 'A', .second = 'B', .separateRecords = true });
    if (!font) return;

    CHECK(font->HasColourGlyph(font->GlyphIndex('A')));
    CHECK(font->HasColourGlyph(font->GlyphIndex('B')));
    CHECK(HasColour(font->Rasterize(font->GlyphIndex('A'), 40.0f), 220, 40, 40));
    CHECK(HasColour(font->Rasterize(font->GlyphIndex('B'), 40.0f), 40, 80, 220));
    // A glyph outside every record is an ordinary letter, not an empty colour glyph.
    CHECK(!font->HasColourGlyph(font->GlyphIndex('C')));
    CHECK_EQ(font->Rasterize(font->GlyphIndex('C'), 40.0f).channels, 1u);
}

TEST(otsvg, a_single_glyph_document_needs_no_id_to_be_found) {
    // One document, one glyph, no wrapper naming it. Keeping nothing because nothing matched
    // would lose the drawing entirely.
    const auto font = Svg({ .letter = 'A', .anonymous = true });
    if (!font) return;

    CHECK(font->HasColourGlyph(font->GlyphIndex('A')));
    CHECK(HasColour(font->Rasterize(font->GlyphIndex('A'), 40.0f), 220, 40, 40));
}

TEST(otsvg, a_drawing_that_is_all_current_colour_stays_an_outline) {
    // `currentColor` means "whatever colour the text is", and the atlas — keyed by face, glyph
    // and size — has nowhere to put that. Same exit as a COLR glyph painted only in the
    // foreground: it is drawn as coverage and tinted like any other letter.
    const auto font = Svg({ .letter = 'A', .currentColour = true });
    if (!font) return;

    const u32 glyph = font->GlyphIndex('A');
    CHECK(!font->HasColourGlyph(glyph));
    CHECK(!font->ColourCovers('A'));
    CHECK_EQ(font->Rasterize(glyph, 40.0f).channels, 1u);
}

TEST(otsvg, a_document_that_is_not_a_drawing_leaves_the_glyph_alone) {
    const auto font = Svg({ .letter = 'A', .instead = "this is not an SVG document" });
    if (!font) return;

    // The table is still read — it is well formed — but the glyph it names draws nothing, so it
    // falls back to its outline rather than to an empty picture.
    const u32 glyph = font->GlyphIndex('A');
    CHECK(!font->HasColourGlyph(glyph));
    const GlyphBitmap bitmap = font->Rasterize(glyph, 40.0f);
    CHECK_EQ(bitmap.channels, 1u);
    CHECK(bitmap.width > 0u);
}

TEST(otsvg, the_rest_of_the_font_is_still_an_ordinary_font) {
    const auto font = Svg({ .letter = 'A' });
    if (!font) return;

    const GlyphMetrics letter = font->Glyph(font->GlyphIndex('M'), 24.0f);
    CHECK(!letter.blank);
    CHECK(letter.advance > 0.0f);
    CHECK_EQ(font->Rasterize(font->GlyphIndex('M'), 24.0f).channels, 1u);
    CHECK(font->Metrics(24.0f).LineHeight() > 0.0f);
}

TEST(otsvg, a_glyph_painted_from_a_style_block_is_still_painted) {
    // Which is how the exporters that make these fonts write them: `.c0{fill:#DC2828}` at the top
    // of the document and `class="c0"` on the shape. A reader that skips `<style>` draws every
    // glyph in the default black and reports nothing wrong.
    const auto font = Svg({ .letter = 'A', .second = 'B', .gzip = true, .viaClass = true });
    if (!font) return;

    CHECK(font->HasColourGlyph(font->GlyphIndex('A')));
    const GlyphBitmap first  = font->Rasterize(font->GlyphIndex('A'), 40.0f);
    const GlyphBitmap second = font->Rasterize(font->GlyphIndex('B'), 40.0f);
    CHECK(HasColour(first, 220, 40, 40));
    CHECK(!HasColour(first, 0, 0, 0, 20));
    CHECK(HasColour(second, 40, 80, 220));
}
