#include "vaepch.h"
#include "vae/text/GlyphAtlas.h"

namespace vae::text {

    namespace {
        // 1px of empty space around each glyph so bilinear sampling at the edge cannot pick up a
        // neighbouring glyph's coverage.
        constexpr u32 kPadding = 1;

        u64 KeyFor(const Font& font, u32 glyph, f32 pixelSize) {
            const u64 face = reinterpret_cast<u64>(&font) >> 4;
            const u64 size = static_cast<u64>(pixelSize * 4.0f);   // quarter-pixel buckets
            return (face << 40) ^ (size << 24) ^ glyph;
        }
    }

    bool GlyphAtlas::Init(gpu::Device& device) {
        m_Device = &device;
        // Only the coverage page up front. A colour page is 4 MB and most apps have no emoji in
        // them, so the first one is made when the first colour glyph asks for it.
        return AddPage(false);
    }

    void GlyphAtlas::Shutdown() {
        m_Pages.clear();
        m_Entries.clear();
        m_Device = nullptr;
    }

    bool GlyphAtlas::AddPage(bool colour) {
        if (m_Pages.size() >= kMaxPages) return false;

        gpu::TextureDesc desc;
        desc.width = desc.height = kPageSize;
        // Coverage for outlines, where the colour comes from the instance; RGBA for a colour font,
        // where the glyph is a picture and the instance has no say in what colour it is.
        desc.format = colour ? gpu::Format::RGBA8_UNORM : gpu::Format::R8_UNORM;
        desc.debugName = colour ? "glyph-atlas-colour" : "glyph-atlas";
        Page page;
        page.colour = colour;
        page.texture = m_Device->CreateTexture(desc);
        if (!page.texture) return false;

        // Zero the page: an unwritten region sampled through padding would otherwise be garbage.
        const std::vector<u8> blank(static_cast<std::size_t>(kPageSize) * kPageSize
                                    * (colour ? 4u : 1u), 0);
        page.texture->Upload(blank.data(), blank.size());

        m_Pages.push_back(std::move(page));
        return true;
    }

    bool GlyphAtlas::Place(u32 width, u32 height, bool colour, u32& outPage, u32& outX, u32& outY) {
        const u32 w = width + kPadding * 2;
        const u32 h = height + kPadding * 2;
        if (w > kPageSize || h > kPageSize) return false;

        for (u32 i = 0; i < m_Pages.size(); ++i) {
            Page& page = m_Pages[i];
            if (page.colour != colour) continue;    // a picture cannot go on a coverage page
            if (page.penX + w > kPageSize) {          // shelf full: start a new one
                page.shelfY += page.shelfHeight;
                page.shelfHeight = 0;
                page.penX = 0;
            }
            if (page.shelfY + h > kPageSize) continue;  // page full

            outPage = i;
            outX = page.penX + kPadding;
            outY = page.shelfY + kPadding;
            page.penX += w;
            page.shelfHeight = std::max(page.shelfHeight, h);
            return true;
        }

        if (!AddPage(colour)) return false;
        return Place(width, height, colour, outPage, outX, outY);
    }

    const GlyphAtlas::Entry* GlyphAtlas::Get(const Font& font, u32 glyph, f32 pixelSize) {
        const u64 key = KeyFor(font, glyph, pixelSize);
        if (auto it = m_Entries.find(key); it != m_Entries.end()) return &it->second;

        const GlyphMetrics metrics = font.Glyph(glyph, pixelSize);
        Entry entry;
        entry.bearing = metrics.bearing;
        entry.size = metrics.size;
        entry.blank = metrics.blank;

        if (metrics.blank) return &m_Entries.emplace(key, entry).first->second;

        const GlyphBitmap bitmap = font.Rasterize(glyph, pixelSize);
        if (bitmap.width == 0 || bitmap.height == 0) {
            entry.blank = true;
            return &m_Entries.emplace(key, entry).first->second;
        }

        entry.colour = bitmap.Colour();
        u32 page = 0, x = 0, y = 0;
        if (!Place(bitmap.width, bitmap.height, entry.colour, page, x, y)) {
            if (!m_WarnedFull) {
                VAE_CORE_ERROR("glyph atlas full ({} pages of {}px, {} glyphs) — text will be missing",
                               m_Pages.size(), kPageSize, m_Entries.size());
                m_WarnedFull = true;
            }
            return nullptr;
        }

        m_Pages[page].texture->UploadRegion(bitmap.pixels.data(), x, y, bitmap.width, bitmap.height);

        constexpr f32 kInv = 1.0f / static_cast<f32>(kPageSize);
        entry.page = page;
        entry.uv = Rect{ { static_cast<f32>(x) * kInv, static_cast<f32>(y) * kInv },
                         { static_cast<f32>(bitmap.width) * kInv,
                           static_cast<f32>(bitmap.height) * kInv } };
        entry.size = { static_cast<f32>(bitmap.width), static_cast<f32>(bitmap.height) };
        entry.blank = false;
        return &m_Entries.emplace(key, entry).first->second;
    }

}
