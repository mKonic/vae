#pragma once

#include "vae/text/TextLayout.h"
#include "vae/gpu/Device.h"

#include <unordered_map>

namespace vae::text {

    // Rasterized glyphs packed into GPU textures, rasterized on demand.
    //
    // Keyed by (face, codepoint, quantized pixel size) rather than pre-baking a glyph range: a
    // design tool has no fixed set of sizes, and a Nerd Font's PUA icons live far outside any range
    // worth pre-baking. This is the model ImGui 1.92 moved to, and why icon glyphs "just work".
    class GlyphAtlas {
    public:
        struct Entry {
            Rect uv;                  // normalized, within the page
            Vec2 size{ 0.0f, 0.0f };  // pixels
            Vec2 bearing{ 0.0f, 0.0f };
            u32  page = 0;
            bool blank = true;
        };

        static constexpr u32 kPageSize = 1024;
        static constexpr u32 kMaxPages = 8;

        bool Init(gpu::Device& device);
        void Shutdown();

        // Null only when the atlas is genuinely out of room, which is logged once.
        const Entry* Get(const Font& font, u32 codepoint, f32 pixelSize);

        const Ref<gpu::Texture>& PageTexture(u32 index) const { return m_Pages[index].texture; }
        u32 PageCount() const { return static_cast<u32>(m_Pages.size()); }
        u32 GlyphCount() const { return static_cast<u32>(m_Entries.size()); }

    private:
        struct Page {
            Ref<gpu::Texture> texture;
            // Shelf packing: glyphs at one size have near-identical heights, so shelves waste very
            // little and cost none of stb_rect_pack's bookkeeping.
            u32 shelfY = 0;
            u32 shelfHeight = 0;
            u32 penX = 0;
        };

        bool Place(u32 width, u32 height, u32& outPage, u32& outX, u32& outY);
        bool AddPage();

        gpu::Device* m_Device = nullptr;
        std::vector<Page> m_Pages;
        std::unordered_map<u64, Entry> m_Entries;
        bool m_WarnedFull = false;
    };

}
