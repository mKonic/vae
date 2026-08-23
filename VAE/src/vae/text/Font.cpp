#include "vaepch.h"
#include "vae/text/Font.h"

#include "vae/base/FileSystem.h"

#include <stb_truetype.h>

#include <hb.h>

namespace vae::text {

    struct FontImpl {
        stbtt_fontinfo info{};
    };

    // HarfBuzz's view of the same bytes. The face is size-independent and built once; a font is a
    // face at a size, and shaping asks for one every call, so they are kept per size rather than
    // rebuilt. Both are reference-counted C objects, hence the explicit destroys.
    struct ShaperFace {
        hb_face_t* face = nullptr;
        std::unordered_map<u32, hb_font_t*> fonts;   // keyed by size in quarter-pixels

        ~ShaperFace() {
            for (auto& [size, font] : fonts) hb_font_destroy(font);
            if (face) hb_face_destroy(face);
        }
    };

    Font::~Font() = default;

    Ref<Font> Font::LoadFromFile(const std::filesystem::path& path) {
        auto bytes = FileSystem::ReadBinary(path);
        if (!bytes) {
            VAE_CORE_ERROR("font not found: {}", path.string());
            return nullptr;
        }
        return LoadFromMemory(std::move(*bytes), path.stem().string());
    }

    Ref<Font> Font::LoadFromMemory(std::vector<u8> data, std::string name) {
        auto font = CreateRef<Font>();
        font->m_Data = std::move(data);
        font->m_Name = std::move(name);
        if (!font->Init()) return nullptr;
        return font;
    }

    // Is this tag one of the face's tables? Walked by hand because stb_truetype has already
    // refused the file, so none of its helpers are available on it.
    bool Font::HasTable(const char* tag) const {
        if (m_Data.size() < 12) return false;
        const auto u16 = [this](std::size_t at) -> u32 {
            return (static_cast<u32>(m_Data[at]) << 8) | m_Data[at + 1];
        };
        const u32 tables = u16(4);
        if (m_Data.size() < 12 + tables * 16u) return false;
        for (u32 i = 0; i < tables; ++i) {
            const std::size_t entry = 12 + static_cast<std::size_t>(i) * 16;
            if (std::memcmp(m_Data.data() + entry, tag, 4) == 0) return true;
        }
        return false;
    }

    bool Font::Init() {
        m_Impl = CreateScope<FontImpl>();
        const int offset = stbtt_GetFontOffsetForIndex(m_Data.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&m_Impl->info, m_Data.data(), offset)) {
            // A colour-bitmap face (CBDT/CBLC, sbix) is not a broken font, it is a font this
            // rasterizer does not read — every Linux box has Noto Color Emoji, and shouting about
            // it in red on every single launch is how a log stops being read at all.
            const bool colourBitmap = HasTable("CBDT") || HasTable("sbix") || HasTable("SVG ");
            if (colourBitmap) VAE_CORE_INFO("'{}' is a colour-bitmap font, which this rasterizer "
                                            "does not draw — skipping it", m_Name);
            else              VAE_CORE_ERROR("'{}' is not a font stb_truetype can read", m_Name);
            return false;
        }
        // Read the real family and style out of the `name` table. Deriving them from the filename
        // is guesswork that gets compound brand names wrong — "JetBrainsMono" splits to
        // "Jet Brains Mono" under any CamelCase heuristic.
        auto NameString = [&](int nameID) -> std::string {
            int length = 0;
            // Windows/Unicode-BMP/English-US is the encoding essentially every font ships.
            const char* raw = stbtt_GetFontNameString(&m_Impl->info, &length, 3, 1, 0x409, nameID);
            if (!raw || length <= 0) return {};
            // UTF-16BE; UI font names are ASCII in practice, so take the low byte of each unit.
            std::string out;
            out.reserve(static_cast<std::size_t>(length) / 2);
            for (int i = 1; i < length; i += 2)
                if (raw[i]) out.push_back(raw[i]);
            return out;
        };

        m_Family = NameString(16);              // typographic family, when present
        if (m_Family.empty()) m_Family = NameString(1);
        m_Style  = NameString(17);
        if (m_Style.empty()) m_Style = NameString(2);
        if (m_Family.empty()) m_Family = m_Name;

        return true;
    }

    void* Font::ShaperFont(f32 pixelSize) const {
        if (!m_Shaper) {
            m_Shaper = CreateScope<ShaperFace>();
            // Blob over our own buffer, not a copy: the Font outlives every hb object it hands out,
            // so HB_MEMORY_MODE_READONLY is exactly true and saves a second copy of every face.
            hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(m_Data.data()),
                                             static_cast<unsigned>(m_Data.size()),
                                             HB_MEMORY_MODE_READONLY, nullptr, nullptr);
            m_Shaper->face = hb_face_create(blob, 0);
            hb_blob_destroy(blob);
        }

        const u32 key = static_cast<u32>(pixelSize * 4.0f);
        if (const auto it = m_Shaper->fonts.find(key); it != m_Shaper->fonts.end()) return it->second;

        hb_font_t* font = hb_font_create(m_Shaper->face);
        // Advances come back in 26.6-style fixed point at this scale; matching the em scale to
        // upem * pixelSize / upem keeps HarfBuzz's numbers in the same units stb_truetype gives us.
        const unsigned upem = hb_face_get_upem(m_Shaper->face);
        const auto scale = static_cast<int>(pixelSize * 64.0f);
        hb_font_set_scale(font, scale, scale);
        hb_font_set_ppem(font, static_cast<unsigned>(pixelSize), static_cast<unsigned>(pixelSize));
        VAE_CORE_ASSERT(upem > 0, "font has no units-per-em");
        m_Shaper->fonts.emplace(key, font);
        return font;
    }

    f32 Font::Scale(f32 pixelSize) const {
        // Em-based, not ascent+descent-based. A "16px" font means a 16px em in CSS, in Figma and
        // in every design tool, so stbtt_ScaleForPixelHeight (which normalises ascent-to-descent)
        // would make every size in an imported design come out slightly too small.
        return stbtt_ScaleForMappingEmToPixels(&m_Impl->info, pixelSize);
    }

    FontMetrics Font::Metrics(f32 pixelSize) const {
        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&m_Impl->info, &ascent, &descent, &lineGap);
        const f32 scale = Scale(pixelSize);

        FontMetrics m;
        // stb reports ascent positive-up; UI space is y-down, so the sign flips here once rather
        // than at every call site.
        m.ascent  = -static_cast<f32>(ascent)  * scale;
        m.descent = -static_cast<f32>(descent) * scale;
        m.lineGap =  static_cast<f32>(lineGap) * scale;
        return m;
    }

    // cmap lookups are not free and the same handful of characters is measured every frame, so the
    // mapping is memoized alongside the metrics it feeds.
    u32 Font::GlyphIndex(u32 codepoint) const {
        if (const auto it = m_IndexCache.find(codepoint); it != m_IndexCache.end()) return it->second;
        const auto index = static_cast<u32>(
            stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(codepoint)));
        m_IndexCache.emplace(codepoint, index);
        return index;
    }

    GlyphMetrics Font::Glyph(u32 glyph, f32 pixelSize) const {
        const u64 key = (static_cast<u64>(glyph) << 20)
                      | static_cast<u64>(static_cast<u32>(pixelSize * 16.0f) & 0xFFFFFu);
        if (auto it = m_MetricsCache.find(key); it != m_MetricsCache.end()) return it->second;

        const f32 scale = Scale(pixelSize);
        const auto index = static_cast<int>(glyph);

        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&m_Impl->info, index, &advance, &bearing);

        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetGlyphBitmapBox(&m_Impl->info, index, scale, scale, &x0, &y0, &x1, &y1);

        GlyphMetrics m;
        m.advance = static_cast<f32>(advance) * scale;
        m.bearing = { static_cast<f32>(x0), static_cast<f32>(y0) };   // y0 is already y-down from stb
        m.size    = { static_cast<f32>(x1 - x0), static_cast<f32>(y1 - y0) };
        m.blank   = (x1 <= x0 || y1 <= y0);

        m_MetricsCache.emplace(key, m);
        return m;
    }

    f32 Font::Kerning(u32 leftGlyph, u32 rightGlyph, f32 pixelSize) const {
        if (!leftGlyph || !rightGlyph) return 0.0f;
        const auto a = static_cast<int>(leftGlyph);
        const auto b = static_cast<int>(rightGlyph);
        return static_cast<f32>(stbtt_GetGlyphKernAdvance(&m_Impl->info, a, b)) * Scale(pixelSize);
    }

    GlyphBitmap Font::Rasterize(u32 glyph, f32 pixelSize) const {
        const f32 scale = Scale(pixelSize);
        const auto index = static_cast<int>(glyph);

        int width = 0, height = 0, xoff = 0, yoff = 0;
        u8* pixels = stbtt_GetGlyphBitmap(&m_Impl->info, scale, scale, index,
                                          &width, &height, &xoff, &yoff);
        GlyphBitmap bitmap;
        if (!pixels) return bitmap;

        bitmap.width  = static_cast<u32>(width);
        bitmap.height = static_cast<u32>(height);
        bitmap.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height);
        stbtt_FreeBitmap(pixels, nullptr);
        return bitmap;
    }

}
