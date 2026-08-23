#include "vaepch.h"
#include "vae/text/Font.h"

#include "vae/base/FileSystem.h"

#include <stb_truetype.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <hb.h>
#include <hb-ot.h>

#include <cmath>

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

    // A colour face's glyphs, as CBLC says where to find them in CBDT.
    //
    // CBLC is an index and CBDT is the pictures. The index is a list of strikes (one, in every
    // colour font that ships), each a list of ranges of glyph ids, each range saying how to turn a
    // glyph id into an offset. Only what real fonts use is read: index formats 1 and 3, which are
    // per-glyph offset arrays, and image formats 17, 18 and 19, which are PNG.
    struct ColourStrike {
        struct Range {
            u32 first = 0, last = 0;
            u16 indexFormat = 0, imageFormat = 0;
            u32 dataOffset = 0;               // into CBDT
            std::vector<u32> offsets;         // one per glyph in the range, plus a final end
        };
        std::vector<Range> ranges;
        u32 cbdt = 0, cbdtLength = 0;
        u32 ppem = 0;
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

    // Where a table is, walked by hand because stb_truetype has already refused the file and none
    // of its helpers are available on it. Offset and length, or {0,0} when there is no such table.
    std::pair<u32, u32> Font::TableRange(const char* tag) const {
        if (m_Data.size() < 12) return { 0, 0 };
        const auto Read16 = [this](std::size_t at) -> u32 {
            return (static_cast<u32>(m_Data[at]) << 8) | m_Data[at + 1];
        };
        const auto Read32 = [this](std::size_t at) -> u32 {
            return (static_cast<u32>(m_Data[at]) << 24) | (static_cast<u32>(m_Data[at + 1]) << 16)
                 | (static_cast<u32>(m_Data[at + 2]) << 8) | m_Data[at + 3];
        };
        const u32 tables = Read16(4);
        if (m_Data.size() < 12 + static_cast<std::size_t>(tables) * 16u) return { 0, 0 };
        for (u32 i = 0; i < tables; ++i) {
            const std::size_t entry = 12 + static_cast<std::size_t>(i) * 16;
            if (std::memcmp(m_Data.data() + entry, tag, 4) != 0) continue;
            const u32 offset = Read32(entry + 8), length = Read32(entry + 12);
            if (offset > m_Data.size() || length > m_Data.size() - offset) return { 0, 0 };
            return { offset, length };
        }
        return { 0, 0 };
    }

    bool Font::HasTable(const char* tag) const { return TableRange(tag).second != 0; }

    // CBLC, parsed. Everything a colour face needs that is not shaping — shaping is HarfBuzz's,
    // which reads this file perfectly well because none of what it needs lives in `glyf`.
    bool Font::InitColour() {
        const auto [cblc, cblcLength] = TableRange("CBLC");
        const auto [cbdt, cbdtLength] = TableRange("CBDT");
        if (!cblcLength || !cbdtLength || cblcLength < 8) return false;

        const auto Read16 = [&](std::size_t at) -> u32 {
            return at + 1 < m_Data.size() ? (static_cast<u32>(m_Data[at]) << 8) | m_Data[at + 1] : 0;
        };
        const auto Read32 = [&](std::size_t at) -> u32 {
            return at + 3 < m_Data.size()
                 ? (static_cast<u32>(m_Data[at]) << 24) | (static_cast<u32>(m_Data[at + 1]) << 16)
                 | (static_cast<u32>(m_Data[at + 2]) << 8) | m_Data[at + 3] : 0;
        };

        const u32 strikes = Read32(cblc + 4);
        if (strikes == 0) return false;

        auto strike = CreateScope<ColourStrike>();
        strike->cbdt = cbdt;
        strike->cbdtLength = cbdtLength;

        // The largest strike, because a UI asks for sizes far below the one size these fonts ship
        // at and scaling down is the direction that keeps detail.
        u32 best = 0, bestPpem = 0;
        for (u32 i = 0; i < strikes; ++i) {
            const std::size_t record = cblc + 8 + static_cast<std::size_t>(i) * 48;
            const u32 ppem = record + 44 < m_Data.size() ? m_Data[record + 44] : 0;
            if (ppem > bestPpem) { bestPpem = ppem; best = i; }
        }
        if (bestPpem == 0) return false;
        strike->ppem = bestPpem;

        const std::size_t record = cblc + 8 + static_cast<std::size_t>(best) * 48;
        const u32 arrayOffset = Read32(record);
        const u32 subtables   = Read32(record + 8);

        for (u32 i = 0; i < subtables; ++i) {
            const std::size_t entry = cblc + arrayOffset + static_cast<std::size_t>(i) * 8;
            ColourStrike::Range range;
            range.first = Read16(entry);
            range.last  = Read16(entry + 2);
            const std::size_t header = cblc + arrayOffset + Read32(entry + 4);
            if (header + 8 > m_Data.size() || range.last < range.first) continue;

            range.indexFormat = static_cast<u16>(Read16(header));
            range.imageFormat = static_cast<u16>(Read16(header + 2));
            range.dataOffset  = Read32(header + 4);

            const u32 count = range.last - range.first + 1;
            if (range.indexFormat == 1) {
                for (u32 g = 0; g <= count; ++g) range.offsets.push_back(Read32(header + 8 + g * 4));
            } else if (range.indexFormat == 3) {
                for (u32 g = 0; g <= count; ++g) range.offsets.push_back(Read16(header + 8 + g * 2));
            } else {
                continue;    // formats 2, 4 and 5 are constant-size strikes; no shipping font uses them here
            }
            strike->ranges.push_back(std::move(range));
        }

        if (strike->ranges.empty()) return false;
        m_Strike = std::move(strike);
        return true;
    }

    bool Font::Init() {
        m_Impl = CreateScope<FontImpl>();
        const int offset = stbtt_GetFontOffsetForIndex(m_Data.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&m_Impl->info, m_Data.data(), offset)) {
            // Not a broken font: a colour face has no `glyf` table at all, so nothing that reads
            // outlines can read it. Its glyphs are pictures, and they are read from CBDT instead.
            if (InitColour()) {
                m_Colour = true;
                // The name table is read through HarfBuzz here rather than through stb, which has
                // no font to read it from.
                auto* face = static_cast<hb_font_t*>(ShaperFont(16.0f));
                if (face) {
                    char buffer[128];
                    unsigned size = sizeof(buffer);
                    if (hb_ot_name_get_utf8(hb_font_get_face(face), HB_OT_NAME_ID_FONT_FAMILY,
                                            HB_LANGUAGE_INVALID, &size, buffer) && size > 0)
                        m_Family.assign(buffer, size);
                }
                if (m_Family.empty()) m_Family = m_Name;
                VAE_CORE_INFO("'{}' is a colour font: {} glyph ranges at {}px",
                              m_Family, m_Strike->ranges.size(), m_Strike->ppem);
                return true;
            }
            const bool otherColour = HasTable("sbix") || HasTable("SVG ") || HasTable("COLR");
            if (otherColour) VAE_CORE_INFO("'{}' stores colour glyphs in a format this build does "
                                           "not read (sbix, SVG or COLR) — skipping it", m_Name);
            else             VAE_CORE_ERROR("'{}' is not a font stb_truetype can read", m_Name);
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

    // The bytes of one glyph's picture, and the metrics CBDT stores in front of it.
    namespace {
        struct ColourEntry {
            const u8* png = nullptr;
            std::size_t length = 0;
            i32 width = 0, height = 0;      // pixels, at the strike's ppem
            i32 bearingX = 0, bearingY = 0; // pixels, y-up from the baseline
        };

        ColourEntry FindColour(const std::vector<u8>& data, const ColourStrike& strike, u32 glyph) {
            ColourEntry entry;
            for (const auto& range : strike.ranges) {
                if (glyph < range.first || glyph > range.last) continue;
                const u32 index = glyph - range.first;
                if (index + 1 >= range.offsets.size()) return entry;

                const std::size_t begin = strike.cbdt + range.dataOffset + range.offsets[index];
                const std::size_t end   = strike.cbdt + range.dataOffset + range.offsets[index + 1];
                if (end <= begin || end > data.size()) return entry;

                // Image formats 17 and 18 put the glyph's metrics in front of the PNG; 19 has them
                // in the index instead and starts with the length. Only the header size differs.
                std::size_t header = 0;
                if (range.imageFormat == 17) {          // smallGlyphMetrics + u32 length + PNG
                    if (end - begin < 9) return entry;
                    entry.height   = data[begin];
                    entry.width    = data[begin + 1];
                    entry.bearingX = static_cast<i8>(data[begin + 2]);
                    entry.bearingY = static_cast<i8>(data[begin + 3]);
                    header = 9;
                } else if (range.imageFormat == 18) {   // bigGlyphMetrics + u32 length + PNG
                    if (end - begin < 12) return entry;
                    entry.height   = data[begin];
                    entry.width    = data[begin + 1];
                    entry.bearingX = static_cast<i8>(data[begin + 2]);
                    entry.bearingY = static_cast<i8>(data[begin + 3]);
                    header = 12;
                } else if (range.imageFormat == 19) {   // u32 length + PNG
                    if (end - begin < 4) return entry;
                    header = 4;
                } else {
                    return entry;
                }

                entry.png = data.data() + begin + header;
                entry.length = end - begin - header;
                return entry;
            }
            return entry;
        }
    }

    // How much a strike bitmap has to shrink to be drawn at this size. The pictures ship at one
    // large ppem and a UI asks for 14 or 16, so this is almost always a downscale.
    static f32 ColourScale(const ColourStrike& strike, f32 pixelSize) {
        return strike.ppem > 0 ? pixelSize / static_cast<f32>(strike.ppem) : 1.0f;
    }

    GlyphMetrics Font::ColourGlyph(u32 glyph, f32 pixelSize) const {
        GlyphMetrics m;
        const ColourEntry entry = FindColour(m_Data, *m_Strike, glyph);
        if (!entry.png || entry.width <= 0 || entry.height <= 0) return m;

        const f32 scale = ColourScale(*m_Strike, pixelSize);
        m.size = { static_cast<f32>(entry.width) * scale, static_cast<f32>(entry.height) * scale };
        // CBDT's bearing is y-up from the baseline to the top of the picture; everything here is
        // y-down from the baseline, so the sign flips exactly once.
        m.bearing = { static_cast<f32>(entry.bearingX) * scale,
                     -static_cast<f32>(entry.bearingY) * scale };

        // The advance comes from hmtx through the shaper, not from the bitmap: it is what shaping
        // already used to place this glyph, and the two disagreeing would draw emoji off their pen.
        if (auto* font = static_cast<hb_font_t*>(ShaperFont(pixelSize)))
            m.advance = static_cast<f32>(hb_font_get_glyph_h_advance(font, glyph)) / 64.0f;
        m.blank = false;
        return m;
    }

    GlyphBitmap Font::RasterizeColour(u32 glyph, f32 pixelSize) const {
        GlyphBitmap bitmap;
        const ColourEntry entry = FindColour(m_Data, *m_Strike, glyph);
        if (!entry.png || entry.length == 0) return bitmap;

        int width = 0, height = 0, channels = 0;
        u8* decoded = stbi_load_from_memory(entry.png, static_cast<int>(entry.length),
                                            &width, &height, &channels, 4);
        if (!decoded || width <= 0 || height <= 0) { stbi_image_free(decoded); return bitmap; }

        const f32 scale = ColourScale(*m_Strike, pixelSize);
        const auto target = [&](int value) {
            return std::max(1, static_cast<int>(std::lround(static_cast<f32>(value) * scale)));
        };
        const int outWidth = target(width), outHeight = target(height);

        bitmap.channels = 4;
        bitmap.width  = static_cast<u32>(outWidth);
        bitmap.height = static_cast<u32>(outHeight);
        bitmap.pixels.resize(static_cast<std::size_t>(outWidth) * outHeight * 4);

        if (outWidth == width && outHeight == height) {
            std::memcpy(bitmap.pixels.data(), decoded, bitmap.pixels.size());
        } else {
            // Premultiplied, because resampling straight alpha bleeds the colour of transparent
            // pixels into the edge — which on emoji is a dark halo around everything.
            stbir_resize_uint8_srgb(decoded, width, height, 0,
                                    bitmap.pixels.data(), outWidth, outHeight, 0,
                                    STBIR_RGBA_PM);
        }
        stbi_image_free(decoded);
        return bitmap;
    }

    FontMetrics Font::Metrics(f32 pixelSize) const {
        if (m_Colour) {
            FontMetrics m;
            if (auto* font = static_cast<hb_font_t*>(ShaperFont(pixelSize))) {
                hb_font_extents_t extents{};
                hb_font_get_h_extents(font, &extents);
                m.ascent  = -static_cast<f32>(extents.ascender)  / 64.0f;
                m.descent = -static_cast<f32>(extents.descender) / 64.0f;
                m.lineGap =  static_cast<f32>(extents.line_gap)  / 64.0f;
            }
            return m;
        }

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
        u32 index = 0;
        if (m_Colour) {
            // No cmap reader of our own: the shaper already has one, and it is the same table.
            hb_codepoint_t found = 0;
            if (auto* font = static_cast<hb_font_t*>(ShaperFont(16.0f)))
                if (hb_font_get_nominal_glyph(font, codepoint, &found)) index = found;
        } else {
            index = static_cast<u32>(stbtt_FindGlyphIndex(&m_Impl->info, static_cast<int>(codepoint)));
        }
        m_IndexCache.emplace(codepoint, index);
        return index;
    }

    GlyphMetrics Font::Glyph(u32 glyph, f32 pixelSize) const {
        if (m_Colour) return ColourGlyph(glyph, pixelSize);
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
        if (m_Colour || !leftGlyph || !rightGlyph) return 0.0f;
        const auto a = static_cast<int>(leftGlyph);
        const auto b = static_cast<int>(rightGlyph);
        return static_cast<f32>(stbtt_GetGlyphKernAdvance(&m_Impl->info, a, b)) * Scale(pixelSize);
    }

    GlyphBitmap Font::Rasterize(u32 glyph, f32 pixelSize) const {
        if (m_Colour) return RasterizeColour(glyph, pixelSize);
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
