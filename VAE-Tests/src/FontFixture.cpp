#include "FontFixture.h"

#include "vae/base/FileSystem.h"

#include <stb_truetype.h>

// The one place in the project that writes a PNG. The engine only ever reads them, so the encoder
// is instantiated here rather than in the engine's stb translation unit.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <algorithm>
#include <cstring>
#include <map>

namespace vae::fixture {

    namespace {

        void Put16(std::vector<u8>& out, u32 value) {
            out.push_back(static_cast<u8>(value >> 8));
            out.push_back(static_cast<u8>(value));
        }
        void Put32(std::vector<u8>& out, u32 value) {
            out.push_back(static_cast<u8>(value >> 24));
            out.push_back(static_cast<u8>(value >> 16));
            out.push_back(static_cast<u8>(value >> 8));
            out.push_back(static_cast<u8>(value));
        }
        u32 Be16(const std::vector<u8>& data, std::size_t at) {
            return (static_cast<u32>(data[at]) << 8) | data[at + 1];
        }
        u32 Be32(const std::vector<u8>& data, std::size_t at) {
            return (static_cast<u32>(data[at]) << 24) | (static_cast<u32>(data[at + 1]) << 16)
                 | (static_cast<u32>(data[at + 2]) << 8) | data[at + 3];
        }

        struct Table {
            std::string     tag;
            std::vector<u8> data;
        };

        // Rebuilds a font file with extra tables in it. Every offset in the directory has to be
        // recomputed rather than patched: adding entries grows the directory itself, so all of the
        // existing table data moves. Entries are written sorted by tag, which the spec requires and
        // which a reader that bisects the directory depends on.
        std::vector<u8> Rebuild(const std::vector<u8>& source, const std::vector<Table>& extra) {
            if (source.size() < 12) return {};

            std::vector<Table> tables;
            const u32 count = Be16(source, 4);
            if (source.size() < 12 + static_cast<std::size_t>(count) * 16) return {};
            for (u32 i = 0; i < count; ++i) {
                const std::size_t entry = 12 + static_cast<std::size_t>(i) * 16;
                const u32 offset = Be32(source, entry + 8), length = Be32(source, entry + 12);
                if (offset > source.size() || length > source.size() - offset) return {};
                Table table;
                table.tag.assign(reinterpret_cast<const char*>(source.data() + entry), 4);
                table.data.assign(source.begin() + offset, source.begin() + offset + length);
                tables.push_back(std::move(table));
            }
            // A tag that is already there is replaced, not duplicated.
            for (const Table& add : extra) {
                const auto it = std::ranges::find(tables, add.tag, &Table::tag);
                if (it != tables.end()) *it = add;
                else                    tables.push_back(add);
            }
            std::ranges::sort(tables, {}, &Table::tag);

            const u32 total = static_cast<u32>(tables.size());
            std::vector<u8> out;
            Put32(out, 0x00010000);                 // version 1.0 — TrueType outlines
            Put16(out, total);
            // searchRange / entrySelector / rangeShift. No reader here uses them, but a font with
            // nonsense in them is a font somebody else's tool would reject.
            u32 power = 1, selector = 0;
            while (power * 2 <= total) { power *= 2; ++selector; }
            Put16(out, power * 16);
            Put16(out, selector);
            Put16(out, (total - power) * 16);

            // Offsets are known only once the directory's size is: 12 + 16 per table, then the
            // data, each table padded to a 4-byte boundary.
            std::size_t at = 12 + static_cast<std::size_t>(total) * 16;
            std::vector<std::size_t> offsets;
            for (const Table& table : tables) {
                offsets.push_back(at);
                at += (table.data.size() + 3) & ~std::size_t(3);
            }

            for (std::size_t i = 0; i < tables.size(); ++i) {
                out.insert(out.end(), tables[i].tag.begin(), tables[i].tag.end());
                // The checksum is left at zero. Nothing that reads these fixtures verifies one —
                // and a wrong value would be worse than an absent one.
                Put32(out, 0);
                Put32(out, static_cast<u32>(offsets[i]));
                Put32(out, static_cast<u32>(tables[i].data.size()));
            }
            for (const Table& table : tables) {
                out.insert(out.end(), table.data.begin(), table.data.end());
                while (out.size() % 4) out.push_back(0);
            }
            return out;
        }

        // A solid square inside a transparent margin, as PNG bytes.
        std::vector<u8> SolidPng(u32 size, u8 r, u8 g, u8 b, u32 border) {
            std::vector<u8> pixels(static_cast<std::size_t>(size) * size * 4, 0);
            for (u32 y = border; y + border < size; ++y)
                for (u32 x = border; x + border < size; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
                    pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = 255;
                }
            std::vector<u8> png;
            stbi_write_png_to_func(
                [](void* context, void* data, int length) {
                    auto* out = static_cast<std::vector<u8>*>(context);
                    const auto* bytes = static_cast<const u8*>(data);
                    out->insert(out->end(), bytes, bytes + length);
                },
                &png, static_cast<int>(size), static_cast<int>(size), 4, pixels.data(),
                static_cast<int>(size) * 4);
            return png;
        }

        // The glyph id a character maps to in the base font. The fixtures name characters because
        // that is what a test can read; the tables want ids.
        u32 GlyphFor(const std::vector<u8>& font, char letter) {
            stbtt_fontinfo info{};
            if (!stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0)))
                return 0;
            return static_cast<u32>(stbtt_FindGlyphIndex(&info, letter));
        }

        u32 GlyphCount(const std::vector<u8>& font) {
            stbtt_fontinfo info{};
            if (!stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0)))
                return 0;
            return static_cast<u32>(info.numGlyphs);
        }

    }

    const std::vector<u8>& BaseFont() {
        static const std::vector<u8> bytes = [] {
            const auto path = FileSystem::Asset("VAE/assets/fonts/JetBrainsMonoNerdFont-Regular.ttf");
            auto data = FileSystem::ReadBinary(path);
            return data ? std::move(*data) : std::vector<u8>{};
        }();
        return bytes;
    }

    std::vector<u8> SbixFont(const SbixSpec& spec) {
        const std::vector<u8>& base = BaseFont();
        if (base.empty()) return {};

        const u32 glyphs = GlyphCount(base);
        const u32 target = GlyphFor(base, spec.letter);
        if (!glyphs || !target) return {};

        const std::vector<u8> png = SolidPng(spec.size, spec.red, spec.green, spec.blue,
                                             spec.border);

        // What each glyph that has one contributes, in glyph order. The offset array is
        // non-decreasing, so an entry that sorted before an earlier one would produce a strike no
        // reader could walk.
        std::map<u32, std::vector<u8>> payloads;
        {
            std::vector<u8> picture;
            Put16(picture, static_cast<u32>(static_cast<u16>(static_cast<i16>(spec.originX))));
            Put16(picture, static_cast<u32>(static_cast<u16>(static_cast<i16>(spec.originY))));
            picture.insert(picture.end(), { 'p', 'n', 'g', ' ' });
            picture.insert(picture.end(), png.begin(), png.end());
            payloads.emplace(target, std::move(picture));
        }
        if (spec.duplicate) {
            const u32 alias = GlyphFor(base, spec.duplicate);
            if (alias) {
                std::vector<u8> dupe;
                Put16(dupe, 0);
                Put16(dupe, 0);
                dupe.insert(dupe.end(), { 'd', 'u', 'p', 'e' });
                Put16(dupe, target);                          // draw that glyph's picture instead
                payloads.emplace(alias, std::move(dupe));
            }
        }

        // The strike: a header, then one offset per glyph plus a terminator, then the data those
        // offsets point into. A glyph with the same offset as its neighbour is how the format
        // spells "no picture here", which is nearly every glyph in a real font.
        std::vector<u8> strike;
        Put16(strike, spec.ppem);
        Put16(strike, 72);                                    // ppi
        std::size_t at = 4 + (static_cast<std::size_t>(glyphs) + 1) * 4;
        std::vector<u8> data;
        for (u32 g = 0; g <= glyphs; ++g) {
            Put32(strike, static_cast<u32>(at));
            if (const auto it = payloads.find(g); it != payloads.end()) {
                data.insert(data.end(), it->second.begin(), it->second.end());
                at += it->second.size();
            }
        }
        strike.insert(strike.end(), data.begin(), data.end());

        Table sbix;
        sbix.tag = "sbix";
        Put16(sbix.data, 1);                                  // version
        Put16(sbix.data, 1);                                  // flags: draw outlines as well
        Put32(sbix.data, 1);                                  // one strike
        Put32(sbix.data, 12);                                 // and it starts right after this
        sbix.data.insert(sbix.data.end(), strike.begin(), strike.end());

        return Rebuild(base, { sbix });
    }

    std::vector<u8> ColrFont(char base, const std::vector<ColrLayerSpec>& layers,
                             const std::vector<PaletteEntry>& palette) {
        const std::vector<u8>& source = BaseFont();
        if (source.empty() || layers.empty() || palette.empty()) return {};

        const u32 target = GlyphFor(source, base);
        if (!target) return {};

        Table colr;
        colr.tag = "COLR";
        Put16(colr.data, 0);                                  // version 0
        Put16(colr.data, 1);                                  // one base glyph record
        Put32(colr.data, 14);                                 // ...immediately after the header
        Put32(colr.data, 14 + 6);                             // then the layer records
        Put16(colr.data, static_cast<u32>(layers.size()));
        Put16(colr.data, target);
        Put16(colr.data, 0);                                  // first layer index
        Put16(colr.data, static_cast<u32>(layers.size()));
        for (const ColrLayerSpec& layer : layers) {
            Put16(colr.data, GlyphFor(source, layer.glyph));
            Put16(colr.data, layer.palette);
        }

        Table cpal;
        cpal.tag = "CPAL";
        Put16(cpal.data, 0);                                  // version 0
        Put16(cpal.data, static_cast<u32>(palette.size()));   // entries per palette
        Put16(cpal.data, 1);                                  // one palette
        Put16(cpal.data, static_cast<u32>(palette.size()));   // colour records
        Put32(cpal.data, 14);                                 // where they start
        Put16(cpal.data, 0);                                  // palette 0 begins at record 0
        for (const PaletteEntry& colour : palette) {
            // BGRA on disk, which is the swap a reader gets silently wrong.
            cpal.data.push_back(colour.b);
            cpal.data.push_back(colour.g);
            cpal.data.push_back(colour.r);
            cpal.data.push_back(colour.a);
        }

        return Rebuild(source, { colr, cpal });
    }

}
