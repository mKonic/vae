#include "Test.h"

#include "vae/base/FileSystem.h"
#include "vae/text/TextLayout.h"

#include <string>

using namespace vae;
using namespace vae::text;

// Shaping: what a codepoint-to-glyph mapping cannot do.
//
// Every assertion here is about a script where the glyphs drawn are not the glyphs the characters
// name — a ligature standing in for two characters, an Arabic letter taking a different form
// because of its neighbour, a Devanagari vowel drawn on the far side of the consonant it follows,
// a mark with no width of its own. The two Noto faces are test fixtures under VAE-Tests/assets,
// not engine assets: the vendored JetBrains Mono has no Arabic and no Devanagari to be wrong with.

namespace {

    Ref<Font> Arabic() {
        static Ref<Font> font = Font::LoadFromFile(
            FileSystem::Asset("VAE-Tests/assets/fonts/NotoSansArabic-Regular.ttf"));
        return font;
    }

    Ref<Font> Devanagari() {
        static Ref<Font> font = Font::LoadFromFile(
            FileSystem::Asset("VAE-Tests/assets/fonts/NotoSansDevanagari-Regular.ttf"));
        return font;
    }

    Ref<Font> Mono() {
        static Ref<Font> font = Font::LoadFromFile(
            FileSystem::Asset("VAE/assets/fonts/JetBrainsMonoNerdFont-Regular.ttf"));
        return font;
    }

    TextStyle StyleFor(const Ref<Font>& font, f32 size = 32.0f) {
        TextStyle style;
        style.font = font;
        style.size = size;
        return style;
    }

    TextLayoutResult Shape(const Ref<Font>& font, std::string_view text, f32 maxWidth = 0.0f,
                           WrapMode wrap = WrapMode::None) {
        return TextLayout::Layout(text, StyleFor(font), maxWidth, wrap);
    }

    // Written as escapes rather than as literals so the source file reads left to right in every
    // editor — a bidi string inline here would display in an order that is not the order it is in.
    const std::string kSalam  = "سلام";      // س ل ا م
    const std::string kSeenFathaLam = "سَل";      // seen, fatha above it, lam
    const std::string kKi     = "कि";                  // ka, then the i-matra

}

TEST(shaping, the_fixture_faces_are_present) {
    CHECK(Arabic() != nullptr);
    CHECK(Devanagari() != nullptr);
    CHECK(Mono() != nullptr);
}

TEST(shaping, a_ligature_replaces_the_glyphs_it_is_made_of) {
    // JetBrains Mono draws "->" as one arrow through `calt`. Neither glyph that comes out is the
    // glyph the cmap gives for the character that went in.
    const auto result = Shape(Mono(), "->");
    CHECK_EQ(result.glyphs.size(), std::size_t(2));
    CHECK(result.glyphs[0].glyph != Mono()->GlyphIndex('-'));
    CHECK(result.glyphs[1].glyph != Mono()->GlyphIndex('>'));
    // Monospace: the pair still occupies exactly two cells.
    CHECK_NEAR(result.glyphs[0].advance, result.glyphs[1].advance);
}

TEST(shaping, arabic_letters_take_their_joined_forms) {
    const auto result = Shape(Arabic(), kSalam);
    CHECK_EQ(result.glyphs.size(), std::size_t(4));

    // Seen and lam both join on both sides here, so neither can be the isolated form the cmap
    // names. This is the whole difference between shaping and a lookup table.
    bool joined = false;
    for (const auto& glyph : result.glyphs)
        if (glyph.codepoint == 0x0633 && glyph.glyph != Arabic()->GlyphIndex(0x0633)) joined = true;
    CHECK(joined);

    for (const auto& glyph : result.glyphs)
        if (glyph.codepoint == 0x0644) CHECK(glyph.glyph != Arabic()->GlyphIndex(0x0644));
}

TEST(shaping, arabic_reads_right_to_left) {
    const auto result = Shape(Arabic(), kSalam);
    CHECK_EQ(result.glyphs.size(), std::size_t(4));

    // Visual order, left to right on screen, is the reverse of the order the bytes are in.
    CHECK_EQ(result.glyphs.front().codepoint, u32(0x0645));   // meem, typed last, drawn leftmost
    CHECK_EQ(result.glyphs.back().codepoint,  u32(0x0633));   // seen, typed first, drawn rightmost
    for (std::size_t i = 1; i < result.glyphs.size(); ++i)
        CHECK(result.glyphs[i].byteOffset < result.glyphs[i - 1].byteOffset);
    // And each one is still placed further right than the last.
    for (std::size_t i = 1; i < result.glyphs.size(); ++i)
        CHECK(result.glyphs[i].pen.x > result.glyphs[i - 1].pen.x);
}

TEST(shaping, a_mark_has_no_width_of_its_own) {
    const auto result = Shape(Arabic(), kSeenFathaLam);
    CHECK_EQ(result.glyphs.size(), std::size_t(3));

    const PositionedGlyph* fatha = nullptr;
    for (const auto& glyph : result.glyphs) if (glyph.codepoint == 0x064E) fatha = &glyph;
    CHECK(fatha != nullptr);
    if (!fatha) return;

    // A mark does not advance the pen — it is positioned onto the letter beside it, which is what
    // the shaper's offset is for. Without that offset it would sit at the pen and miss the letter.
    CHECK_NEAR(fatha->advance, 0.0f);
    CHECK(fatha->offset.x != 0.0f);
}

TEST(shaping, a_devanagari_matra_is_drawn_before_the_consonant_it_follows) {
    const auto result = Shape(Devanagari(), kKi);
    CHECK_EQ(result.glyphs.size(), std::size_t(2));

    // कि is typed consonant-then-vowel and drawn vowel-then-consonant. No amount of iterating
    // codepoints in order produces this; the shaper reorders the syllable.
    CHECK_EQ(result.glyphs[1].glyph, Devanagari()->GlyphIndex(0x0915));   // ka, second
    CHECK(result.glyphs[0].glyph != Devanagari()->GlyphIndex(0x0915));    // the matra, first
    CHECK(result.glyphs[0].pen.x < result.glyphs[1].pen.x);
}

TEST(shaping, a_fallback_face_starts_a_new_run) {
    TextStyle style = StyleFor(Mono());
    style.fallbacks.push_back(Arabic());

    const auto result = TextLayout::Layout("Hi " + kSalam, style, 0.0f, WrapMode::None);
    CHECK_EQ(result.glyphs.size(), std::size_t(7));

    // Latin on the primary, Arabic on the fallback, and every glyph resolved by something.
    CHECK(result.glyphs[0].face == Mono().get());
    CHECK(result.glyphs[1].face == Mono().get());
    CHECK(result.glyphs.back().face == Arabic().get());
    for (const auto& glyph : result.glyphs) CHECK(glyph.glyph != 0u);
}

TEST(shaping, an_arabic_word_inside_an_english_sentence_stays_where_it_was_typed) {
    TextStyle style = StyleFor(Mono());
    style.fallbacks.push_back(Arabic());

    const auto result = TextLayout::Layout("Hi " + kSalam, style, 0.0f, WrapMode::None);
    // The paragraph's direction comes from its first strong character, so this one is left to
    // right: "Hi" on the left, the Arabic word to the right of it, reversed inside itself.
    CHECK_EQ(result.glyphs.front().codepoint, u32('H'));
    CHECK_NEAR(result.glyphs.front().pen.x, 0.0f);
    CHECK_EQ(result.glyphs.back().codepoint, u32(0x0633));
}

TEST(shaping, a_right_to_left_paragraph_puts_its_first_word_on_the_right) {
    TextStyle style = StyleFor(Mono());
    style.fallbacks.push_back(Arabic());

    const auto result = TextLayout::Layout(kSalam + " Hi", style, 0.0f, WrapMode::None);
    CHECK_EQ(result.glyphs.size(), std::size_t(7));

    // Typed Arabic-then-Latin, so the paragraph is right to left and the Arabic — logically first —
    // is drawn on the right, with the Latin to its left.
    CHECK_EQ(result.glyphs.front().codepoint, u32('H'));
    CHECK_NEAR(result.glyphs.front().pen.x, 0.0f);

    f32 latinRight = 0.0f, arabicLeft = 1e9f;
    for (const auto& glyph : result.glyphs) {
        if (glyph.codepoint == 'H' || glyph.codepoint == 'i')
            latinRight = std::max(latinRight, glyph.pen.x + glyph.advance);
        if (glyph.codepoint >= 0x0600 && glyph.codepoint <= 0x06FF)
            arabicLeft = std::min(arabicLeft, glyph.pen.x);
    }
    CHECK(latinRight <= arabicLeft);
}

TEST(shaping, right_to_left_text_wraps_on_words_and_keeps_its_line_order) {
    const std::string three = kSalam + " " + kSalam + " " + kSalam;
    const f32 oneWord = Shape(Arabic(), kSalam).size.x;

    const auto result = Shape(Arabic(), three, oneWord + 1.0f, WrapMode::Word);
    CHECK_EQ(result.lines.size(), std::size_t(3));

    // Lines run in logical order even though the glyphs inside each one do not: the first line
    // holds the first word. Breaking the visual order instead would put the last word on top.
    const std::size_t wordBytes = kSalam.size();
    for (u32 i = 0; i < result.lines[0].glyphCount; ++i)
        CHECK(result.glyphs[result.lines[0].firstGlyph + i].byteOffset <= wordBytes);
    for (u32 i = 0; i < result.lines[2].glyphCount; ++i)
        CHECK(result.glyphs[result.lines[2].firstGlyph + i].byteOffset >= 2 * (wordBytes + 1));

    // And no line is wider than the box it was given.
    for (const auto& line : result.lines) CHECK(line.width <= oneWord + 1.0f);
}

TEST(shaping, a_space_at_the_edge_does_not_take_a_line_of_its_own) {
    // A break-space that overflows is trimmed off the line's width, so wrapping on it would leave
    // a line holding nothing but that space.
    const auto style = StyleFor(Mono(), 16.0f);
    const f32 width = TextLayout::Measure("aaaa", style).x;
    const auto result = TextLayout::Layout("aaaa bbbb", style, width, WrapMode::Word);

    CHECK_EQ(result.lines.size(), std::size_t(2));
    for (const auto& line : result.lines) CHECK(line.glyphCount > 1u);
}

TEST(shaping, a_space_between_two_directions_stays_between_them) {
    TextStyle style = StyleFor(Mono());
    style.fallbacks.push_back(Arabic());

    // "Hello <arabic> world". The space after the Arabic word is neutral, and the Arabic run is
    // laid out right to left — absorbed into that run it would be drawn on the far side of the
    // word, which reads on screen as a missing space.
    const auto result = TextLayout::Layout("Hello " + kSalam + " world", style, 0.0f, WrapMode::None);

    f32 latinLeftEnd = 0.0f, arabicLeft = 1e9f, arabicRight = 0.0f, latinRightStart = 1e9f;
    std::size_t spaces = 0;
    for (const auto& glyph : result.glyphs) {
        if (glyph.codepoint == ' ') { ++spaces; continue; }
        const bool arabic = glyph.codepoint >= 0x0600 && glyph.codepoint <= 0x06FF;
        if (arabic) {
            arabicLeft  = std::min(arabicLeft,  glyph.pen.x);
            arabicRight = std::max(arabicRight, glyph.pen.x + glyph.advance);
        } else if (glyph.byteOffset < 6) {
            latinLeftEnd = std::max(latinLeftEnd, glyph.pen.x + glyph.advance);
        } else {
            latinRightStart = std::min(latinRightStart, glyph.pen.x);
        }
    }

    CHECK_EQ(spaces, std::size_t(2));
    CHECK(latinLeftEnd < arabicLeft);
    CHECK(arabicRight < latinRightStart);
    // A real gap on both sides, not two words touching.
    CHECK(arabicLeft - latinLeftEnd > 1.0f);
    CHECK(latinRightStart - arabicRight > 1.0f);
}
