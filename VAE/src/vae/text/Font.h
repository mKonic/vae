#pragma once

#include "vae/base/Base.h"
#include "vae/base/Math.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace vae::text {

    // Metrics scaled to a pixel size, baseline-relative and y-down: ascent is negative (above the
    // baseline), descent positive.
    struct FontMetrics {
        f32 ascent = 0.0f;
        f32 descent = 0.0f;
        f32 lineGap = 0.0f;
        f32 LineHeight() const { return descent - ascent + lineGap; }
    };

    struct GlyphMetrics {
        f32 advance = 0.0f;
        Vec2 bearing{ 0.0f, 0.0f };   // pen position to the bitmap's top-left
        Vec2 size{ 0.0f, 0.0f };      // bitmap size in pixels
        bool blank = true;            // no coverage (space, or an unmapped codepoint)
    };

    struct GlyphBitmap {
        std::vector<u8> pixels;       // 8-bit coverage, or RGBA when channels is 4
        u32 width = 0, height = 0;
        u32 channels = 1;
        bool Colour() const { return channels == 4; }
    };

    // How a face stores colour glyphs, when it has any. Four formats exist and three are read
    // here. They are not variations on one idea: two are pictures and one is drawing instructions.
    enum class ColourFormat {
        None,
        Cbdt,   // CBLC indexes CBDT: PNGs at one big ppem. Noto Color Emoji, and so every Linux.
        Sbix,   // Apple's: PNGs again, indexed per strike per glyph, and the face keeps its outlines.
        Colr,   // Not pictures at all — a base glyph is a list of other glyphs, each in a palette
                // colour (COLR/CPAL). Composited here rather than drawn as layers; see Font.cpp.
    };

    // A single face at arbitrary sizes. Rasterization is CPU-only with no GPU dependency, which is
    // what lets text measurement be unit-tested headlessly — layout depends on measurement, so if
    // measuring needed a device then so would every layout test.
    class Font {
    public:
        static Ref<Font> LoadFromFile(const std::filesystem::path& path);
        static Ref<Font> LoadFromMemory(std::vector<u8> data, std::string name);
        ~Font();

        // Whether this face can produce colour glyphs at all, and how it stores them. Not the same
        // question as whether a *particular* glyph is coloured: a CBDT emoji face has a picture for
        // everything it covers, but a COLR face draws most of its glyphs as ordinary outlines and
        // only some as layers.
        bool Colour() const { return m_ColourFormat != ColourFormat::None; }
        ColourFormat ColourStorage() const { return m_ColourFormat; }
        bool HasColourGlyph(u32 glyph) const;
        // The same question asked of a character rather than a glyph, which is what font selection
        // wants: "does this face draw U+1F600 in colour", not "does it have some outline for it".
        bool ColourCovers(u32 codepoint) const {
            const u32 glyph = GlyphIndex(codepoint);
            return glyph != 0 && HasColourGlyph(glyph);
        }

        // Whether the face has outlines to rasterize. False for CBDT emoji faces, which have no
        // `glyf` table at all — that is why stb_truetype refuses them and why everything they
        // answer comes from HarfBuzz and the bitmap strike instead.
        bool Outlines() const { return m_Outlines; }

        const std::string& Name() const { return m_Name; }
        // From the font's own `name` table, not from the filename.
        const std::string& FamilyName() const { return m_Family; }
        const std::string& StyleName()  const { return m_Style; }

        FontMetrics  Metrics(f32 pixelSize) const;

        // Everything below the mapping takes a *glyph index*, not a codepoint. A shaper's output is
        // glyph indices — one glyph can come from several codepoints (a ligature) and one codepoint
        // from several glyphs (a decomposed mark) — so a codepoint-keyed metric or atlas entry has
        // nothing to key on once shaping is real.
        u32 GlyphIndex(u32 codepoint) const;
        bool HasGlyph(u32 codepoint) const { return GlyphIndex(codepoint) != 0; }

        GlyphMetrics Glyph(u32 glyph, f32 pixelSize) const;
        f32          Kerning(u32 leftGlyph, u32 rightGlyph, f32 pixelSize) const;
        GlyphBitmap  Rasterize(u32 glyph, f32 pixelSize) const;

        // The bytes the face was loaded from, for a shaper that wants its own view of the file.
        const std::vector<u8>& Data() const { return m_Data; }
        // Opaque `hb_font_t*`, created on first use and owned by the font. Null when HarfBuzz is
        // not compiled in.
        void* ShaperFont(f32 pixelSize) const;

    private:
        bool Init();
        bool InitCbdt();
        bool InitSbix();
        bool InitColr();
        bool HasTable(const char* tag) const;
        std::pair<u32, u32> TableRange(const char* tag) const;
        f32  Scale(f32 pixelSize) const;
        // CBDT and sbix are both "a PNG and where to put it", so they share the two calls that
        // turn one into metrics and pixels; only finding it differs.
        GlyphMetrics PictureGlyph(u32 glyph, f32 pixelSize) const;
        GlyphBitmap  RasterizePicture(u32 glyph, f32 pixelSize) const;
        GlyphMetrics ColrGlyph(u32 glyph, f32 pixelSize) const;
        GlyphBitmap  RasterizeColr(u32 glyph, f32 pixelSize) const;

        std::vector<u8> m_Data;
        std::string     m_Name;
        std::string     m_Family;
        std::string     m_Style;
        // Held type-erased so stb_truetype.h stays out of this header — it is ~5000 lines and
        // every TU that included it would pay for it.
        Scope<struct FontImpl> m_Impl;

        mutable std::unordered_map<u64, GlyphMetrics> m_MetricsCache;
        mutable std::unordered_map<u32, u32>          m_IndexCache;
        mutable Scope<struct ShaperFace>              m_Shaper;
        Scope<struct ColourStrike>                    m_Strike;   // CBDT
        Scope<struct SbixStrike>                      m_Sbix;
        Scope<struct ColrLayers>                      m_Colr;
        ColourFormat                                  m_ColourFormat = ColourFormat::None;
        bool                                          m_Outlines = false;
        // Where this font's table directory is. Non-zero only in a collection ('ttcf'), which is
        // how Apple ships the colour face `sbix` exists for.
        u32                                           m_Sfnt = 0;
    };

}
