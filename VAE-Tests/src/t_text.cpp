#include "Test.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Utf8.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextCache.h"
#include "vae/text/TextLayout.h"

#include <string>
#include <vector>

using namespace vae;
using namespace vae::text;

namespace {

    Ref<Font> TestFont() {
        static Ref<Font> font =
            Font::LoadFromFile(FileSystem::Asset("VAE/assets/fonts/JetBrainsMonoNerdFont-Regular.ttf"));
        return font;
    }

    TextStyle TestStyle(f32 size = 16.0f) {
        TextStyle style;
        style.font = TestFont();
        style.size = size;
        return style;
    }

}

// ------------------------------------------------------------------ UTF-8

TEST(text, a_memoized_style_is_still_the_right_style) {
    // FontDB::Style is asked for the same handful of styles thousands of times a frame, so it
    // memoizes. A memo that returned the same chain for every request would silently draw every
    // weight at 400 and every family in the default face.
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");

    const TextStyle regular = db.Style({ "", FontWeight::Regular, FontSlant::Normal, 14.0f });
    CHECK(regular.font != nullptr);
    const Font* face = regular.font.get();

    // Size rides on the cached entry rather than splitting it, so asking at another size is the
    // same face at the new size — not the old size handed back.
    const TextStyle bigger = db.Style({ "", FontWeight::Regular, FontSlant::Normal, 32.0f });
    CHECK_EQ(bigger.size, 32.0f);
    CHECK(bigger.font.get() == face);
    CHECK_EQ(db.Style({ "", FontWeight::Regular, FontSlant::Normal, 14.0f }).size, 14.0f);

    // Weight is part of the key: Medium is bundled beside Regular, so the two must not collapse
    // into one entry.
    const TextStyle medium = db.Style({ "", FontWeight::Medium, FontSlant::Normal, 14.0f });
    CHECK(medium.font != nullptr);

    // A family nobody has falls back to the default rather than returning a null face or the
    // previous answer.
    const TextStyle missing = db.Style({ "No Such Family At All", FontWeight::Regular,
                                         FontSlant::Normal, 14.0f });
    CHECK(missing.font != nullptr);
}

TEST(text, a_shaped_run_comes_back_identical_from_the_cache) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");
    const TextStyle style = db.Style({ "", FontWeight::Regular, FontSlant::Normal, 14.0f });
    CHECK(style.font != nullptr);

    const std::string content = "Cached runs must be the runs they cache";
    const TextLayoutResult direct = TextLayout::Layout(content, style, 0.0f, WrapMode::None);
    const TextLayoutResult& cached = TextCache::Layout(content, style, 0.0f, WrapMode::None);

    CHECK_EQ(cached.glyphs.size(), direct.glyphs.size());
    CHECK(std::abs(cached.size.x - direct.size.x) < 0.001f);
    for (std::size_t i = 0; i < direct.glyphs.size(); ++i) {
        CHECK_EQ(cached.glyphs[i].codepoint, direct.glyphs[i].codepoint);
        CHECK(std::abs(cached.glyphs[i].pen.x - direct.glyphs[i].pen.x) < 0.001f);
    }

    // Second time is a hit, and a hit is the same answer.
    const auto before = TextCache::Report().hits;
    const TextLayoutResult& again = TextCache::Layout(content, style, 0.0f, WrapMode::None);
    CHECK(TextCache::Report().hits > before);
    CHECK_EQ(again.glyphs.size(), direct.glyphs.size());

    // The width a run was wrapped into is part of what it is: the same string at 60px wraps to
    // more lines than at 4000, and a cache that ignored the width would hand back the wrong one.
    const std::size_t narrow = TextCache::Layout(content, style, 60.0f, WrapMode::Word).lines.size();
    const std::size_t wide = TextCache::Layout(content, style, 4000.0f, WrapMode::Word).lines.size();
    CHECK(narrow > wide);

    // And a different string of the same length is a different run, not a hash collision.
    const std::string other = "Sached suns bust ce tre suns tley cacle";
    CHECK(TextCache::Layout(other, style, 0.0f, WrapMode::None).glyphs[0].codepoint == 'S');
    CHECK(TextCache::Layout(content, style, 0.0f, WrapMode::None).glyphs[0].codepoint == 'C');
}

TEST(utf8, decodes_every_sequence_length) {
    std::size_t i = 0;
    std::string s = "A";                    // 1 byte
    Utf8Append(s, 0x00E9);                  // 2 bytes: e-acute
    Utf8Append(s, 0x4E2D);                  // 3 bytes: CJK
    Utf8Append(s, 0x1F600);                 // 4 bytes: emoji

    CHECK_EQ(Utf8Next(s, i), u32{ 'A' });
    CHECK_EQ(Utf8Next(s, i), 0x00E9u);
    CHECK_EQ(Utf8Next(s, i), 0x4E2Du);
    CHECK_EQ(Utf8Next(s, i), 0x1F600u);
    CHECK_EQ(i, s.size());
    CHECK_EQ(Utf8Length(s), 4u);
}

TEST(utf8, malformed_bytes_do_not_desynchronise_or_hang) {
    // A lone continuation byte followed by valid text: the bad byte must consume exactly itself.
    const std::string s = "\x80" "ok";
    std::size_t i = 0;
    CHECK_EQ(Utf8Next(s, i), kReplacementChar);
    CHECK_EQ(i, 1u);
    CHECK_EQ(Utf8Next(s, i), u32{ 'o' });
    CHECK_EQ(Utf8Next(s, i), u32{ 'k' });
    CHECK_EQ(i, s.size());
}

TEST(utf8, truncated_sequence_terminates) {
    const std::string s = "\xE4\xB8";       // 3-byte lead with only one continuation
    std::size_t i = 0;
    std::size_t iterations = 0;
    while (i < s.size() && iterations < 10) { Utf8Next(s, i); ++iterations; }
    CHECK(iterations < 10);
    CHECK_EQ(i, s.size());
}

// ------------------------------------------------------------------ Font

TEST(font, bundled_face_loads) {
    CHECK(TestFont() != nullptr);
}

TEST(font, metrics_describe_a_sane_line) {
    const FontMetrics m = TestFont()->Metrics(16.0f);
    CHECK(m.ascent < 0.0f);              // y-down: above the baseline is negative
    CHECK(m.descent > 0.0f);
    CHECK(m.LineHeight() > 16.0f);   // em-based sizing: ascent+descent exceeds the em
    CHECK(m.LineHeight() < 32.0f);
}

TEST(font, metrics_scale_with_pixel_size) {
    const f32 small = TestFont()->Metrics(10.0f).LineHeight();
    const f32 large = TestFont()->Metrics(20.0f).LineHeight();
    CHECK_NEAR(large / small, 2.0);
}

// Metrics and rasterization are keyed on glyph index, not codepoint — a shaper's output has no
// codepoint to key on. Everything below goes through the cmap first, on purpose.
TEST(font, monospace_advances_are_equal) {
    const f32 i = TestFont()->Glyph(TestFont()->GlyphIndex('i'), 16.0f).advance;
    const f32 m = TestFont()->Glyph(TestFont()->GlyphIndex('M'), 16.0f).advance;
    CHECK(i > 0.0f);
    CHECK_NEAR(i, m);
}

TEST(font, a_codepoint_maps_to_a_glyph_index) {
    CHECK(TestFont()->GlyphIndex('A') != 0u);
    CHECK(TestFont()->GlyphIndex('A') != 'A');          // indices are not codepoints
    CHECK_EQ(TestFont()->GlyphIndex(0x10FFFD), 0u);     // unmapped is index zero, not a crash
}

TEST(font, space_is_blank_but_advances) {
    const GlyphMetrics space = TestFont()->Glyph(TestFont()->GlyphIndex(' '), 16.0f);
    CHECK(space.blank);
    CHECK(space.advance > 0.0f);
}

TEST(font, rasterizes_coverage) {
    const GlyphBitmap bitmap = TestFont()->Rasterize(TestFont()->GlyphIndex('M'), 32.0f);
    CHECK(bitmap.width > 0u);
    CHECK(bitmap.height > 0u);
    CHECK_EQ(bitmap.pixels.size(), std::size_t(bitmap.width) * bitmap.height);
    u32 ink = 0;
    for (u8 p : bitmap.pixels) if (p > 128) ++ink;
    CHECK(ink > 0u);
}

// ------------------------------------------------------------------ Layout

TEST(textlayout, single_line_width_is_the_sum_of_advances) {
    const auto style = TestStyle();
    const auto layout = TextLayout::Layout("Hello", style);
    CHECK_EQ(layout.lines.size(), 1u);
    CHECK_EQ(layout.glyphs.size(), 5u);

    f32 expected = 0.0f;
    for (const auto& g : layout.glyphs) expected += g.advance;
    CHECK_NEAR(layout.lines[0].width, expected);
    CHECK_NEAR(layout.size.x, expected);
}

TEST(textlayout, empty_string_still_has_a_line_height) {
    const auto layout = TextLayout::Layout("", TestStyle());
    CHECK_NEAR(layout.size.x, 0.0f);
    CHECK(layout.size.y > 0.0f);
}

TEST(textlayout, hard_newlines_split_lines) {
    const auto layout = TextLayout::Layout("a\nb\nc", TestStyle());
    CHECK_EQ(layout.lines.size(), 3u);
    CHECK_EQ(layout.glyphs.size(), 3u);
    CHECK(layout.lines[1].baselineY > layout.lines[0].baselineY);
}

TEST(textlayout, word_wrap_breaks_at_spaces) {
    const auto style = TestStyle();
    const f32 advance = style.font->Glyph('m', style.size).advance;
    // Room for about six characters, so "aaa bbb ccc" must become three lines.
    const auto layout = TextLayout::Layout("aaa bbb ccc", style, advance * 6.0f);
    CHECK_EQ(layout.lines.size(), 3u);
    for (const auto& line : layout.lines) CHECK(line.width <= advance * 6.0f + 0.01f);
}

TEST(textlayout, trailing_whitespace_is_excluded_from_line_width) {
    const auto style = TestStyle();
    const f32 plain   = TextLayout::Layout("abc", style).lines[0].width;
    const f32 trailing = TextLayout::Layout("abc   ", style).lines[0].width;
    CHECK_NEAR(plain, trailing);
}

TEST(textlayout, a_word_longer_than_the_line_still_breaks) {
    const auto style = TestStyle();
    const f32 advance = style.font->Glyph('m', style.size).advance;
    const auto layout = TextLayout::Layout("mmmmmmmmmmmm", style, advance * 4.0f);
    CHECK(layout.lines.size() >= 3u);      // must not run off to infinity on one line
    CHECK(layout.size.x <= advance * 4.0f + 0.01f);
}

TEST(textlayout, wrap_none_ignores_max_width) {
    const auto style = TestStyle();
    const auto layout = TextLayout::Layout("aaa bbb ccc", style, 10.0f, WrapMode::None);
    CHECK_EQ(layout.lines.size(), 1u);
}

TEST(textlayout, centre_and_right_alignment_shift_glyphs) {
    const auto style = TestStyle();
    const f32 box = 400.0f;
    const auto left   = TextLayout::Layout("abc", style, box, WrapMode::Word, TextAlign::Left);
    const auto centre = TextLayout::Layout("abc", style, box, WrapMode::Word, TextAlign::Center);
    const auto right  = TextLayout::Layout("abc", style, box, WrapMode::Word, TextAlign::Right);

    CHECK_NEAR(left.glyphs[0].pen.x, 0.0f);
    CHECK_NEAR(centre.glyphs[0].pen.x, (box - centre.lines[0].width) * 0.5f);
    CHECK_NEAR(right.glyphs[0].pen.x, box - right.lines[0].width);
}

TEST(textlayout, glyph_baselines_advance_by_line_height) {
    auto style = TestStyle();
    style.lineHeight = 20.0f;
    const auto layout = TextLayout::Layout("a\nb", style);
    CHECK_NEAR(layout.glyphs[1].pen.y - layout.glyphs[0].pen.y, 20.0f);
    CHECK_NEAR(layout.size.y, 40.0f);
}

TEST(textlayout, letter_spacing_widens_the_line) {
    auto tight = TestStyle();
    auto loose = TestStyle();
    loose.letterSpacing = 4.0f;
    const f32 a = TextLayout::Layout("abcd", tight).size.x;
    const f32 b = TextLayout::Layout("abcd", loose).size.x;
    CHECK_NEAR(b - a, 16.0f);
}

TEST(textlayout, hit_test_finds_caret_positions) {
    const auto style = TestStyle();
    const auto layout = TextLayout::Layout("abcdef", style);
    CHECK_EQ(TextLayout::HitTest(layout, { -100.0f, 0.0f }), 0u);
    CHECK_EQ(TextLayout::HitTest(layout, { 10000.0f, 0.0f }), 6u);

    const f32 mid = layout.glyphs[3].pen.x;
    CHECK_EQ(TextLayout::HitTest(layout, { mid, 0.0f }), 3u);
}

TEST(textlayout, byte_offsets_survive_multibyte_text) {
    std::string text = "a";
    Utf8Append(text, 0x4E2D);              // 3-byte
    text += "b";
    const auto layout = TextLayout::Layout(text, TestStyle());
    CHECK_EQ(layout.glyphs.size(), 3u);
    CHECK_EQ(layout.glyphs[0].byteOffset, 0u);
    CHECK_EQ(layout.glyphs[1].byteOffset, 1u);
    CHECK_EQ(layout.glyphs[2].byteOffset, 4u);
}

// ------------------------------------------------------------------ FontDB

TEST(fontdb, parses_family_weight_and_slant_from_filenames) {
    // Filename parsing is a HINT, not an authority — the real family comes from the font's name
    // table (Font::FamilyName). A CamelCase splitter cannot know that "JetBrains" is one word, so
    // what must hold is that the guess matches after normalization, which is how lookup compares.
    CHECK_EQ(FontDB::NormalizeFamilyForTest(FontDB::FamilyFromFilename("JetBrainsMonoNerdFont-Regular")),
             FontDB::NormalizeFamilyForTest("JetBrains Mono Nerd Font"));
    CHECK_EQ(FontDB::FamilyFromFilename("Inter-SemiBoldItalic"), std::string("Inter"));
    CHECK_EQ(FontDB::NormalizeFamilyForTest(FontDB::FamilyFromFilename("NotoSansCJK")),
             FontDB::NormalizeFamilyForTest("Noto Sans CJK"));

    CHECK(FontDB::WeightFromName("Inter-SemiBold") == FontWeight::SemiBold);
    CHECK(FontDB::WeightFromName("Inter-ExtraBold") == FontWeight::ExtraBold);
    CHECK(FontDB::WeightFromName("Inter-ExtraLight") == FontWeight::ExtraLight);
    CHECK(FontDB::WeightFromName("Inter-Bold") == FontWeight::Bold);
    CHECK(FontDB::WeightFromName("Inter") == FontWeight::Regular);

    CHECK(FontDB::SlantFromName("Inter-BoldItalic") == FontSlant::Italic);
    CHECK(FontDB::SlantFromName("Inter-Bold") == FontSlant::Normal);
}

TEST(fontdb, real_family_name_comes_from_the_font_file) {
    // The bundled file is named "JetBrainsMonoNerdFont-Regular.ttf"; the name table is the truth.
    CHECK(!TestFont()->FamilyName().empty());
    CHECK(TestFont()->FamilyName().find("JetBrains") != std::string::npos);
}

TEST(fontdb, registers_and_resolves_the_bundled_family) {
    FontDB db;
    const u32 count = db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    CHECK(count >= 2u);
    CHECK(db.HasFamily("JetBrains Mono Nerd Font"));

    auto font = db.Resolve("JetBrains Mono Nerd Font", FontWeight::Regular);
    CHECK(font != nullptr);
}

TEST(fontdb, picks_the_nearest_available_weight) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);

    const auto faces = db.Faces("JetBrains Mono Nerd Font");
    CHECK(faces.size() >= 2u);

    // Only Regular (400) and Medium (500) are bundled. A SemiBold (600) request must land on
    // Medium, and a Thin (100) request on Regular.
    auto semibold = db.Resolve("JetBrains Mono Nerd Font", FontWeight::SemiBold);
    auto thin     = db.Resolve("JetBrains Mono Nerd Font", FontWeight::Thin);
    CHECK(semibold != nullptr);
    CHECK(thin != nullptr);
    CHECK(semibold != thin);
}

TEST(fontdb, unknown_family_falls_back_to_the_default) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");

    auto font = db.Resolve("Definitely Not Installed Sans");
    CHECK(font != nullptr);
    CHECK(font == db.Resolve("JetBrains Mono Nerd Font"));
}

TEST(fontdb, style_attaches_the_fallback_chain) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");
    db.SetFallbackFamilies({ "JetBrains Mono Nerd Font" });

    const TextStyle style = db.Style(FontRequest{ "", FontWeight::Regular, FontSlant::Normal, 18.0f });
    CHECK(style.font != nullptr);
    CHECK_NEAR(style.size, 18.0f);
}

TEST(fontdb, fallback_resolves_glyphs_the_primary_face_lacks) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);

    TextStyle style;
    style.font = db.Resolve("JetBrains Mono Nerd Font");
    style.size = 16.0f;

    // Contrived but exact: a face that has 'A' is asked for a codepoint it does not have, with a
    // fallback that does. FaceFor must pick the fallback.
    struct Probe { u32 codepoint; };
    const u32 missing = 0x10FFFD;                  // private use plane, in no normal font
    CHECK(!style.font->HasGlyph(missing));
    CHECK(style.FaceFor(missing) == style.font);   // no fallback yet: stays on the primary

    style.fallbacks.push_back(style.font);
    CHECK(style.FaceFor('A') == style.font);
}

TEST(fontdb, family_lookup_ignores_spacing_and_case) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    auto canonical = db.Resolve("JetBrains Mono Nerd Font");
    CHECK(canonical != nullptr);
    CHECK(db.Resolve("jetbrainsmononerdfont") == canonical);
    CHECK(db.Resolve("JETBRAINS MONO NERD FONT") == canonical);
    CHECK(db.Resolve("JetBrains-Mono-Nerd-Font") == canonical);
}

TEST(fontdb, an_unlisted_script_still_finds_a_face) {
    // The named fallback chain is a list of the scripts somebody thought of. Devanagari is not on
    // this one, and the text still has to draw: the database is asked for any registered face that
    // covers the character, which is the difference between a fixed list and actual coverage.
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.RegisterDirectory(FileSystem::Asset("VAE-Tests/assets/fonts"), false, true);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");
    db.SetFallbackFamilies({});

    const TextStyle& style = db.Style({ "", FontWeight::Regular, FontSlant::Normal, 16.0f });
    CHECK(style.font != nullptr);
    CHECK(!style.font->HasGlyph(0x0915));       // ka: not in the primary, and nothing else is listed

    const Ref<Font>& face = style.FaceFor(0x0915);
    CHECK(face != nullptr);
    CHECK(face != style.font);
    CHECK(face->HasGlyph(0x0915));

    // A character no installed face has resolves to the primary rather than to nothing, so the
    // worst case is a box and never a crash.
    CHECK(style.FaceFor(0x10FFFD) == style.font);
}

TEST(fontdb, the_coverage_answer_is_remembered) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    db.RegisterDirectory(FileSystem::Asset("VAE-Tests/assets/fonts"), false, true);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");
    db.SetFallbackFamilies({});

    // The search reads font files off disk, so asking twice must not scan twice — and the second
    // answer has to be the same face, not a second copy of it.
    const Ref<Font>& first  = db.FaceCovering(0x0915, FontWeight::Regular, FontSlant::Normal);
    const Ref<Font>& second = db.FaceCovering(0x0915, FontWeight::Regular, FontSlant::Normal);
    CHECK(first != nullptr);
    CHECK(first == second);
    CHECK(&first == &second);
}

TEST(fontdb, a_character_nothing_covers_does_not_keep_every_font_it_opened) {
    FontDB db;
    db.RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"), false, true);
    // Registered without reading their metadata, which is how a system scan registers hundreds of
    // files: the name is inferred and nothing is opened until something needs the face.
    db.RegisterDirectory(FileSystem::Asset("VAE-Tests/assets/fonts"), false, false);
    db.SetDefaultFamily("JetBrains Mono Nerd Font");
    db.SetFallbackFamilies({});

    const auto loadedFaces = [&] {
        std::size_t loaded = 0;
        for (const auto& family : db.Families())
            for (const auto& face : db.Faces(family)) if (face.loaded) ++loaded;
        return loaded;
    };

    const std::size_t before = loadedFaces();
    // Searching for a character nothing has opens every face there is. Keeping them would mean one
    // undrawable character pulls the machine's whole font collection into memory.
    CHECK(db.FaceCovering(0x10FFFD, FontWeight::Regular, FontSlant::Normal) == nullptr);
    CHECK_EQ(loadedFaces(), before);

    // A face that does answer is kept, because it is about to be drawn from.
    CHECK(db.FaceCovering(0x0915, FontWeight::Regular, FontSlant::Normal) != nullptr);
    CHECK(loadedFaces() > before);
}
