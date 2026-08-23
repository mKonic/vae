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
        std::vector<u8> pixels;       // 8-bit coverage
        u32 width = 0, height = 0;
    };

    // A single face at arbitrary sizes. Rasterization is CPU-only with no GPU dependency, which is
    // what lets text measurement be unit-tested headlessly — layout depends on measurement, so if
    // measuring needed a device then so would every layout test.
    class Font {
    public:
        static Ref<Font> LoadFromFile(const std::filesystem::path& path);
        static Ref<Font> LoadFromMemory(std::vector<u8> data, std::string name);
        ~Font();

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
        bool HasTable(const char* tag) const;
        f32  Scale(f32 pixelSize) const;

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
    };

}
