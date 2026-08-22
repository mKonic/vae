#include "vaepch.h"
#include "vae/text/Font.h"

#include "vae/base/FileSystem.h"

#include <stb_truetype.h>

namespace vae::text {

    struct FontImpl {
        stbtt_fontinfo info{};
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

    bool Font::Init() {
        m_Impl = CreateScope<FontImpl>();
        const int offset = stbtt_GetFontOffsetForIndex(m_Data.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&m_Impl->info, m_Data.data(), offset)) {
            VAE_CORE_ERROR("'{}' is not a font stb_truetype can read", m_Name);
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

    bool Font::HasGlyph(u32 codepoint) const {
        return stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(codepoint)) != 0;
    }

    GlyphMetrics Font::Glyph(u32 codepoint, f32 pixelSize) const {
        const u64 key = (static_cast<u64>(codepoint) << 20)
                      | static_cast<u64>(static_cast<u32>(pixelSize * 16.0f) & 0xFFFFFu);
        if (auto it = m_MetricsCache.find(key); it != m_MetricsCache.end()) return it->second;

        const f32 scale = Scale(pixelSize);
        const int index = stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(codepoint));

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

    f32 Font::Kerning(u32 left, u32 right, f32 pixelSize) const {
        const int a = stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(left));
        const int b = stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(right));
        if (!a || !b) return 0.0f;
        return static_cast<f32>(stbtt_GetGlyphKernAdvance(&m_Impl->info, a, b)) * Scale(pixelSize);
    }

    GlyphBitmap Font::Rasterize(u32 codepoint, f32 pixelSize) const {
        const f32 scale = Scale(pixelSize);
        const int index = stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(codepoint));

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
