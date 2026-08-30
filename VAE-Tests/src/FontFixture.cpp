#include "FontFixture.h"

#include "vae/base/FileSystem.h"

#include <stb_truetype.h>

// The one place in the project that writes a PNG. The engine only ever reads them, so the encoder
// is instantiated here rather than in the engine's stb translation unit.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <algorithm>
#include <cstdio>
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

        // The box a glyph draws in, in font units. The gradient fixtures lay themselves across it
        // rather than across the em: a gradient that runs past the shape it fills never reaches its
        // last stop, and a test written that way is checking nothing at its far end.
        bool GlyphBox(const std::vector<u8>& font, u32 glyph, i32& x0, i32& y0, i32& x1, i32& y1) {
            stbtt_fontinfo info{};
            if (!stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0)))
                return false;
            int a = 0, b = 0, c = 0, d = 0;
            if (!stbtt_GetGlyphBox(&info, static_cast<int>(glyph), &a, &b, &c, &d)) return false;
            x0 = a; y0 = b; x1 = c; y1 = d;
            return c > a && d > b;
        }

        u32 GlyphCount(const std::vector<u8>& font) {
            stbtt_fontinfo info{};
            if (!stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0)))
                return 0;
            return static_cast<u32>(info.numGlyphs);
        }

    }

    namespace {

        u32 Crc32(const std::string& text) {
            u32 crc = 0xFFFFFFFFu;
            for (const char byte : text) {
                crc ^= static_cast<u8>(byte);
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        // A real gzip stream, because the reader has to step over a real gzip header to find the
        // deflate data. stb's compressor writes a *zlib* stream — a two-byte header, the deflate
        // data, an adler32 — so the deflate is what is left when both ends come off, and it is
        // rewrapped in gzip's own header and trailer.
        std::vector<u8> Gzip(const std::string& text) {
            int deflatedLength = 0;
            u8* deflated = stbi_zlib_compress(
                reinterpret_cast<u8*>(const_cast<char*>(text.data())),
                static_cast<int>(text.size()), &deflatedLength, 8);
            if (!deflated || deflatedLength <= 6) { STBIW_FREE(deflated); return {}; }

            std::vector<u8> out{ 0x1F, 0x8B, 0x08, 0x00,      // magic, deflate, no optional fields
                                 0x00, 0x00, 0x00, 0x00,      // mtime: a fixture has no date
                                 0x00, 0xFF };                // no extra flags, unknown OS
            out.insert(out.end(), deflated + 2, deflated + deflatedLength - 4);
            STBIW_FREE(deflated);

            const u32 crc = Crc32(text);
            const auto size = static_cast<u32>(text.size());
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(crc >> (i * 8)));
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(size >> (i * 8)));
            return out;
        }

    }

    std::vector<u8> SvgFont(const SvgSpec& spec) {
        const std::vector<u8>& source = BaseFont();
        if (source.empty()) return {};

        const u32 first = GlyphFor(source, spec.letter);
        const u32 second = spec.second ? GlyphFor(source, spec.second) : 0;
        if (!first || (spec.second && !second)) return {};

        // Which class each colour gets, when the fill is coming from a stylesheet.
        const auto className = [&](const PaletteEntry& colour) {
            return "c" + std::to_string(colour.r) + "_" + std::to_string(colour.b);
        };
        const auto paintOf = [&](const PaletteEntry& colour) {
            char paint[32];
            if (spec.currentColour) std::snprintf(paint, sizeof(paint), "currentColor");
            else std::snprintf(paint, sizeof(paint), "#%02X%02X%02X", colour.r, colour.g, colour.b);
            return std::string(paint);
        };
        const auto rect = [&](const PaletteEntry& colour, i32 offset) {
            const std::string paint = spec.viaClass
                                    ? R"(class=")" + className(colour) + '"'
                                    : R"(fill=")" + paintOf(colour) + '"';
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer),
                          R"(<rect x="%d" y="%d" width="%d" height="%d" %s/>)",
                          spec.x + offset, spec.y, spec.width, spec.height, paint.c_str());
            return std::string(buffer);
        };
        const auto sheet = [&] {
            if (!spec.viaClass) return std::string();
            std::string css = "<style type=\"text/css\"><![CDATA[";
            css += "." + className(spec.colour) + "{fill:" + paintOf(spec.colour) + ";}";
            if (spec.second)
                css += "." + className(spec.secondColour) + "{fill:"
                     + paintOf(spec.secondColour) + ";}";
            return css + "]]></style>";
        };
        const auto document = [&](const std::string& body) {
            return R"(<?xml version="1.0" encoding="UTF-8"?>)"
                   R"(<svg xmlns="http://www.w3.org/2000/svg">)" + sheet() + body + "</svg>";
        };
        const auto wrap = [&](u32 glyph, const std::string& body) {
            if (spec.anonymous) return body;
            return "<g id=\"glyph" + std::to_string(glyph) + "\">" + body + "</g>";
        };

        // Each entry is one record: the glyphs it covers, and the bytes of its document.
        struct Entry { u32 first, last; std::vector<u8> bytes; };
        const auto encode = [&](const std::string& text) {
            if (!spec.instead.empty())
                return std::vector<u8>(spec.instead.begin(), spec.instead.end());
            if (spec.gzip) return Gzip(text);
            return std::vector<u8>(text.begin(), text.end());
        };

        std::vector<Entry> entries;
        if (spec.second && spec.separateRecords) {
            entries.push_back({ first, first,
                                encode(document(wrap(first, rect(spec.colour, 0)))) });
            entries.push_back({ second, second,
                                encode(document(wrap(second, rect(spec.secondColour, 0)))) });
        } else if (spec.second) {
            // One document drawing both, which is how a real emoji font ships: the two are found
            // by their ids and not by which record they are in.
            entries.push_back({ std::min(first, second), std::max(first, second),
                                encode(document(wrap(first, rect(spec.colour, 0))
                                                + wrap(second, rect(spec.secondColour, 800)))) });
        } else {
            entries.push_back({ first, first,
                                encode(document(wrap(first, rect(spec.colour, 0)))) });
        }
        for (const Entry& entry : entries)
            if (entry.bytes.empty()) return {};
        std::ranges::sort(entries, {}, &Entry::first);

        Table svg;
        svg.tag = "SVG ";
        Put16(svg.data, 0);                                   // version 0
        Put32(svg.data, 10);                                  // the document list, right after
        Put32(svg.data, 0);                                   // reserved

        // Offsets in the records are from the start of the *list*, which is where the count is.
        u32 at = 2 + static_cast<u32>(entries.size()) * 12;
        Put16(svg.data, static_cast<u32>(entries.size()));
        for (const Entry& entry : entries) {
            Put16(svg.data, entry.first);
            Put16(svg.data, entry.last);
            Put32(svg.data, at);
            Put32(svg.data, static_cast<u32>(entry.bytes.size()));
            at += static_cast<u32>(entry.bytes.size());
        }
        for (const Entry& entry : entries)
            svg.data.insert(svg.data.end(), entry.bytes.begin(), entry.bytes.end());

        return Rebuild(source, { svg });
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

    namespace {

        // CPAL is the same table whichever version of COLR is beside it.
        Table PaletteTable(const std::vector<PaletteEntry>& palette) {
            Table cpal;
            cpal.tag = "CPAL";
            Put16(cpal.data, 0);
            Put16(cpal.data, static_cast<u32>(palette.size()));
            Put16(cpal.data, 1);
            Put16(cpal.data, static_cast<u32>(palette.size()));
            Put32(cpal.data, 14);
            Put16(cpal.data, 0);
            for (const PaletteEntry& colour : palette) {
                cpal.data.push_back(colour.b);      // BGRA on disk
                cpal.data.push_back(colour.g);
                cpal.data.push_back(colour.r);
                cpal.data.push_back(colour.a);
            }
            return cpal;
        }

        // Writing a paint graph is mostly offset arithmetic, and all of it is relative: a paint
        // names its children as 24-bit deltas from its own first byte. So a parent is written with
        // a hole where each child's offset goes, and the hole is filled once the child has been
        // written after it — which is also why children always come later in the table.
        struct Paints {
            std::vector<u8> d;

            u32 Here() const { return static_cast<u32>(d.size()); }
            void Put8(u32 v) { d.push_back(static_cast<u8>(v)); }
            void Put16(u32 v) { fixture::Put16(d, v); }
            void Put32(u32 v) { fixture::Put32(d, v); }
            void Put24(u32 v) {
                d.push_back(static_cast<u8>(v >> 16));
                d.push_back(static_cast<u8>(v >> 8));
                d.push_back(static_cast<u8>(v));
            }
            // F2DOT14 and FWORD, the two number formats the paint tables are written in.
            void PutF2Dot14(f32 v) { Put16(static_cast<u16>(static_cast<i16>(v * 16384.0f))); }
            void PutFword(i32 v) { Put16(static_cast<u16>(static_cast<i16>(v))); }
            void PutFixed(f32 v) { Put32(static_cast<u32>(static_cast<i32>(v * 65536.0f))); }

            u32 Hole24() { const u32 at = Here(); Put24(0); return at; }
            void Fill(u32 hole, u32 parent, u32 child) {
                const u32 delta = child - parent;
                d[hole]     = static_cast<u8>(delta >> 16);
                d[hole + 1] = static_cast<u8>(delta >> 8);
                d[hole + 2] = static_cast<u8>(delta);
            }
            void Fill32(u32 hole, u32 value) {
                d[hole]     = static_cast<u8>(value >> 24);
                d[hole + 1] = static_cast<u8>(value >> 16);
                d[hole + 2] = static_cast<u8>(value >> 8);
                d[hole + 3] = static_cast<u8>(value);
            }
        };

    }

    std::vector<u8> ColrV1Font(const ColrV1Spec& spec, const std::vector<PaletteEntry>& palette) {
        const std::vector<u8>& source = BaseFont();
        if (source.empty() || palette.empty()) return {};

        const u32 base  = GlyphFor(source, spec.base);
        const u32 shape = GlyphFor(source, spec.shape);
        const u32 under = spec.under ? GlyphFor(source, spec.under) : 0;
        if (!base || !shape) return {};

        const bool clipped = spec.clipXMax > spec.clipXMin && spec.clipYMax > spec.clipYMin;
        const bool layered = spec.viaLayers;
        const u32 layerCount = layered ? (under ? 2u : 1u) : 0u;

        Paints p;
        // The header, whose five version-1 offsets are filled in once their tables are placed.
        p.Put16(1);                         // version
        p.Put16(0);                         // no version-0 base glyph records
        p.Put32(0);
        p.Put32(0);
        p.Put16(0);                         // ...and no version-0 layer records
        const u32 baseListHole  = p.Here(); p.Put32(0);
        const u32 layerListHole = p.Here(); p.Put32(0);
        const u32 clipListHole  = p.Here(); p.Put32(0);
        p.Put32(0);                         // varIndexMap
        p.Put32(0);                         // itemVariationStore

        const u32 baseList = p.Here();
        p.Fill32(baseListHole, baseList);
        p.Put32(1);                         // one base glyph paint record
        p.Put16(base);
        const u32 rootHole = p.Here(); p.Put32(0);   // ...from the start of THIS list, not the table

        u32 layerList = 0;
        std::vector<u32> layerHoles;
        if (layered) {
            layerList = p.Here();
            p.Fill32(layerListHole, layerList);
            p.Put32(layerCount);
            for (u32 i = 0; i < layerCount; ++i) { layerHoles.push_back(p.Here()); p.Put32(0); }
        }

        if (clipped) {
            const u32 clipList = p.Here();
            p.Fill32(clipListHole, clipList);
            p.Put8(1);                      // format 1
            p.Put32(1);                      // one record
            p.Put16(base);                   // startGlyphID
            p.Put16(base);                   // endGlyphID
            const u32 boxHole = p.Here(); p.Put24(0);
            const u32 box = p.Here();
            p.Fill(boxHole, clipList, box);
            p.Put8(1);                      // ClipBox format 1
            p.PutFword(spec.clipXMin);
            p.PutFword(spec.clipYMin);
            p.PutFword(spec.clipXMax);
            p.PutFword(spec.clipYMax);
        }

        // --- the graph, outermost first so every child offset points forwards -------------------

        const u32 root = p.Here();
        p.Fill32(rootHole, root - baseList);

        u32 compositeSourceHole = 0, compositeBackdropHole = 0;
        if (spec.composite >= 0) {
            p.Put8(32);
            compositeSourceHole = p.Hole24();
            p.Put8(static_cast<u32>(spec.composite));
            compositeBackdropHole = p.Hole24();
        }

        // What the composite's source is, or the root itself when there is no composite.
        const u32 stackTop = spec.composite >= 0 ? p.Here() : root;
        if (spec.composite >= 0) p.Fill(compositeSourceHole, root, stackTop);

        u32 transformParent = 0, transformHole = 0;
        if (layered) {
            p.Put8(1);                       // PaintColrLayers
            p.Put8(layerCount);
            p.Put32(0);                      // firstLayerIndex
        }

        // The transform, when asked for, sits directly above the glyph.
        const u32 afterLayers = layered ? p.Here() : stackTop;
        u32 fillParent = afterLayers;
        if (spec.transform != ColrV1Spec::Transform::None) {
            transformParent = afterLayers;
            switch (spec.transform) {
                case ColrV1Spec::Transform::Translate:
                    p.Put8(14); transformHole = p.Hole24();
                    p.PutFword(static_cast<i32>(spec.amount));
                    p.PutFword(static_cast<i32>(spec.amount));
                    break;
                case ColrV1Spec::Transform::Scale:
                    p.Put8(16); transformHole = p.Hole24();
                    p.PutF2Dot14(spec.amount);
                    p.PutF2Dot14(spec.amount);
                    break;
                case ColrV1Spec::Transform::Rotate:
                    p.Put8(24); transformHole = p.Hole24();
                    p.PutF2Dot14(spec.amount / 180.0f);
                    break;
                case ColrV1Spec::Transform::Skew:
                    p.Put8(28); transformHole = p.Hole24();
                    p.PutF2Dot14(spec.amount / 180.0f);
                    p.PutF2Dot14(0.0f);
                    break;
                case ColrV1Spec::Transform::Matrix:
                    p.Put8(12);
                    transformHole = p.Hole24();
                    {
                        const u32 matrixHole = p.Hole24();
                        const u32 matrix = p.Here();
                        p.Fill(matrixHole, transformParent, matrix);
                        p.PutFixed(spec.amount); p.PutFixed(0.0f);
                        p.PutFixed(0.0f);        p.PutFixed(spec.amount);
                        p.PutFixed(0.0f);        p.PutFixed(0.0f);
                    }
                    break;
                default: break;
            }
            fillParent = p.Here();
            p.Fill(transformHole, transformParent, fillParent);
        }

        // PaintGlyph, and the fill inside it.
        const u32 glyphPaint = fillParent;
        p.Put8(10);
        const u32 fillHole = p.Hole24();
        p.Put16(shape);
        if (layered && !layerHoles.empty())
            p.Fill32(layerHoles[layerCount - 1], (layered ? afterLayers : glyphPaint) - layerList);

        const u32 fill = p.Here();
        p.Fill(fillHole, glyphPaint, fill);
        switch (spec.fill) {
            case ColrV1Spec::Fill::Solid:
                p.Put8(2);
                p.Put16(spec.palette);
                p.PutF2Dot14(spec.alpha);
                break;
            case ColrV1Spec::Fill::Linear:
            case ColrV1Spec::Fill::Radial:
            case ColrV1Spec::Fill::Sweep: {
                p.Put8(spec.fill == ColrV1Spec::Fill::Linear ? 4
                     : spec.fill == ColrV1Spec::Fill::Radial ? 6 : 8);
                const u32 lineHole = p.Hole24();
                // Across the shape being filled, so both ends of the colour line are actually
                // reached — measured from the glyph rather than guessed at from the em.
                i32 x0 = 0, y0 = 0, x1 = 1000, y1 = 1000;
                GlyphBox(source, shape, x0, y0, x1, y1);
                const i32 cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
                if (spec.fill == ColrV1Spec::Fill::Linear) {
                    // Left to right, with the rotation point straight up so it runs along x.
                    p.PutFword(x0); p.PutFword(y0);          // p0
                    p.PutFword(x1); p.PutFword(y0);          // p1
                    p.PutFword(x0); p.PutFword(y0 + 1000);   // p2
                } else if (spec.fill == ColrV1Spec::Fill::Radial) {
                    const i32 radius = std::max(x1 - x0, y1 - y0) / 2;
                    p.PutFword(cx); p.PutFword(cy); p.PutFword(0);        // inner circle
                    p.PutFword(cx); p.PutFword(cy); p.PutFword(radius);   // outer
                } else {
                    p.PutFword(cx); p.PutFword(cy);      // centre
                    p.PutF2Dot14(0.0f);                  // start angle: 0 degrees
                    p.PutF2Dot14(2.0f);                  // end angle: 360
                }
                const u32 line = p.Here();
                p.Fill(lineHole, fill, line);
                p.Put8(spec.extend);
                p.Put16(2);                              // two stops
                p.PutF2Dot14(0.0f); p.Put16(spec.palette);  p.PutF2Dot14(1.0f);
                p.PutF2Dot14(1.0f); p.Put16(spec.palette2); p.PutF2Dot14(1.0f);
                break;
            }
        }

        // The glyph underneath, which is either the composite's backdrop or the first layer.
        if (under) {
            const u32 beneath = p.Here();
            if (spec.composite >= 0) p.Fill(compositeBackdropHole, root, beneath);
            if (layered && !layerHoles.empty()) p.Fill32(layerHoles[0], beneath - layerList);
            p.Put8(10);
            const u32 solidHole = p.Hole24();
            p.Put16(under);
            const u32 solid = p.Here();
            p.Fill(solidHole, beneath, solid);
            p.Put8(2);
            p.Put16(spec.underPalette);
            p.PutF2Dot14(1.0f);
        }

        Table colr;
        colr.tag = "COLR";
        colr.data = std::move(p.d);
        return Rebuild(source, { colr, PaletteTable(palette) });
    }

}
