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

    // Apple's answer to the same question, and a simpler one. `sbix` is a list of strikes; a strike
    // is one offset per glyph into its own data, and the data is a PNG with an origin in front of
    // it. No index formats, no image formats — and no metrics either, so the picture's size comes
    // from the PNG's own header.
    //
    // The difference that matters is that an sbix face keeps its `glyf` table: stb_truetype reads
    // it, the outlines are real (usually empty boxes), and the colour is an addition rather than a
    // replacement. CBDT faces have no outlines at all.
    struct SbixStrike {
        u32 strike = 0;        // absolute offset of the chosen strike
        u32 ppem = 0;
        u32 glyphs = 0;        // from maxp; the strike has this many offsets, plus a terminator
    };

    // COLR/CPAL: not a picture in any format. A base glyph is a list of *other glyphs in this same
    // font*, each drawn in a colour from a palette, back to front. Version 0 only — version 1 adds
    // a paint graph with gradients and blend modes, whose glyphs live in a different list that is
    // not read here, so a v1-only face draws its outlines and no colour.
    struct ColrLayers {
        struct Base { u32 glyph = 0, first = 0, count = 0; };
        struct Layer { u32 glyph = 0; u32 palette = 0; };
        struct Rgba  { u8 r = 0, g = 0, b = 0, a = 255; };

        std::vector<Base>  bases;      // sorted by glyph, as the table requires
        std::vector<Layer> layers;
        std::vector<Rgba>  palette;    // palette 0; a document does not choose one

        // Layers whose palette index is 0xFFFF are drawn in the *text* colour, which an atlas keyed
        // by (face, glyph, size) cannot vary. See RasterizeColr for what happens instead.
        static constexpr u32 kForeground = 0xFFFFu;

        const Base* Find(u32 glyph) const {
            const auto it = std::lower_bound(bases.begin(), bases.end(), glyph,
                                             [](const Base& b, u32 g) { return b.glyph < g; });
            return it != bases.end() && it->glyph == glyph ? &*it : nullptr;
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

    // Big-endian reads that answer 0 past the end of the buffer rather than reading it. Every
    // table below is parsed straight out of a file somebody else wrote, so "past the end" is a
    // normal input and not a reason to have a bounds check at each of forty call sites.
    namespace {
        u32 Be16(const std::vector<u8>& data, std::size_t at) {
            return at + 1 < data.size() ? (static_cast<u32>(data[at]) << 8) | data[at + 1] : 0;
        }
        u32 Be32(const std::vector<u8>& data, std::size_t at) {
            return at + 3 < data.size()
                 ? (static_cast<u32>(data[at]) << 24) | (static_cast<u32>(data[at + 1]) << 16)
                 | (static_cast<u32>(data[at + 2]) << 8) | data[at + 3] : 0;
        }
        i32 BeS16(const std::vector<u8>& data, std::size_t at) {
            return static_cast<i16>(static_cast<u16>(Be16(data, at)));
        }
    }

    // Where the font's table directory starts. Zero for an ordinary file; a *collection* ('ttcf')
    // holds several fonts in one file and lists where each begins. Apple Color Emoji — the font
    // `sbix` is read for at all — ships as one, so this is not a hypothetical.
    static u32 SfntOffset(const std::vector<u8>& data) {
        if (data.size() < 16 || std::memcmp(data.data(), "ttcf", 4) != 0) return 0;
        return Be32(data, 8) > 0 ? Be32(data, 12) : 0;      // numFonts, then the first one
    }

    // Whether the file is even shaped like a font, checked before stb_truetype is let near it.
    // **`stbtt_InitFont` takes a pointer and no length**: it trusts the table directory and walks
    // it, so a truncated or corrupt file reads straight off the end of the buffer. Every table
    // this class reads for itself is bounds-checked in TableRange; this is that same check for the
    // ones stb reads, done once, before anything is handed to it.
    static bool Plausible(const std::vector<u8>& data, u32 sfnt) {
        if (data.size() < static_cast<std::size_t>(sfnt) + 12) return false;
        const u32 tables = Be16(data, sfnt + 4);
        if (tables == 0 || data.size() < sfnt + 12 + static_cast<std::size_t>(tables) * 16)
            return false;
        for (u32 i = 0; i < tables; ++i) {
            const std::size_t entry = sfnt + 12 + static_cast<std::size_t>(i) * 16;
            const u32 offset = Be32(data, entry + 8), length = Be32(data, entry + 12);
            if (offset > data.size() || length > data.size() - offset) return false;
        }
        return true;
    }

    // Where a table is, walked by hand because stb_truetype has already refused the file and none
    // of its helpers are available on it. Offset and length, or {0,0} when there is no such table.
    // Offsets are from the start of the *file* even in a collection; only the directory moves.
    std::pair<u32, u32> Font::TableRange(const char* tag) const {
        if (m_Data.size() < static_cast<std::size_t>(m_Sfnt) + 12) return { 0, 0 };
        const u32 tables = Be16(m_Data, m_Sfnt + 4);
        if (m_Data.size() < m_Sfnt + 12 + static_cast<std::size_t>(tables) * 16u) return { 0, 0 };
        for (u32 i = 0; i < tables; ++i) {
            const std::size_t entry = m_Sfnt + 12 + static_cast<std::size_t>(i) * 16;
            if (std::memcmp(m_Data.data() + entry, tag, 4) != 0) continue;
            const u32 offset = Be32(m_Data, entry + 8), length = Be32(m_Data, entry + 12);
            if (offset > m_Data.size() || length > m_Data.size() - offset) return { 0, 0 };
            return { offset, length };
        }
        return { 0, 0 };
    }

    bool Font::HasTable(const char* tag) const { return TableRange(tag).second != 0; }

    // CBLC, parsed. Everything a colour face needs that is not shaping — shaping is HarfBuzz's,
    // which reads this file perfectly well because none of what it needs lives in `glyf`.
    bool Font::InitCbdt() {
        const auto [cblc, cblcLength] = TableRange("CBLC");
        const auto [cbdt, cbdtLength] = TableRange("CBDT");
        if (!cblcLength || !cbdtLength || cblcLength < 8) return false;

        const u32 strikes = Be32(m_Data, cblc + 4);
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
        const u32 arrayOffset = Be32(m_Data, record);
        const u32 subtables   = Be32(m_Data, record + 8);

        for (u32 i = 0; i < subtables; ++i) {
            const std::size_t entry = cblc + arrayOffset + static_cast<std::size_t>(i) * 8;
            ColourStrike::Range range;
            range.first = Be16(m_Data, entry);
            range.last  = Be16(m_Data, entry + 2);
            const std::size_t header = cblc + arrayOffset + Be32(m_Data, entry + 4);
            if (header + 8 > m_Data.size() || range.last < range.first) continue;

            range.indexFormat = static_cast<u16>(Be16(m_Data, header));
            range.imageFormat = static_cast<u16>(Be16(m_Data, header + 2));
            range.dataOffset  = Be32(m_Data, header + 4);

            const u32 count = range.last - range.first + 1;
            if (range.indexFormat == 1) {
                for (u32 g = 0; g <= count; ++g) range.offsets.push_back(Be32(m_Data, header + 8 + g * 4));
            } else if (range.indexFormat == 3) {
                for (u32 g = 0; g <= count; ++g) range.offsets.push_back(Be16(m_Data, header + 8 + g * 2));
            } else {
                continue;    // formats 2, 4 and 5 are constant-size strikes; no shipping font uses them here
            }
            strike->ranges.push_back(std::move(range));
        }

        if (strike->ranges.empty()) return false;
        m_Strike = std::move(strike);
        return true;
    }

    bool Font::InitSbix() {
        const auto [sbix, length] = TableRange("sbix");
        if (length < 8) return false;

        // How many offsets a strike has. `maxp` is the only place that says, and getting it wrong
        // means reading a strike's offsets off the end of it — which is why it is read here rather
        // than assumed from the table's size.
        const auto [maxp, maxpLength] = TableRange("maxp");
        const u32 glyphs = maxpLength >= 6 ? Be16(m_Data, maxp + 4) : 0;
        if (glyphs == 0) return false;

        const u32 strikes = Be32(m_Data, sbix + 4);
        if (strikes == 0) return false;

        // The biggest strike, for the same reason CBDT picks one: a UI asks for 14 or 16 px and
        // scaling a large picture down is the direction that keeps detail.
        u32 bestOffset = 0, bestPpem = 0;
        for (u32 i = 0; i < strikes; ++i) {
            const u32 offset = Be32(m_Data, sbix + 8 + static_cast<std::size_t>(i) * 4);
            if (offset == 0 || sbix + offset + 4 > m_Data.size()) continue;
            const u32 ppem = Be16(m_Data, sbix + offset);
            if (ppem > bestPpem) { bestPpem = ppem; bestOffset = sbix + offset; }
        }
        if (bestPpem == 0) return false;

        // The strike must actually hold its offset array, or every lookup reads past the table.
        if (static_cast<std::size_t>(bestOffset) + 4 + (static_cast<std::size_t>(glyphs) + 1) * 4
            > m_Data.size())
            return false;

        auto strike = CreateScope<SbixStrike>();
        strike->strike = bestOffset;
        strike->ppem   = bestPpem;
        strike->glyphs = glyphs;
        m_Sbix = std::move(strike);
        return true;
    }

    bool Font::InitColr() {
        const auto [colr, colrLength] = TableRange("COLR");
        const auto [cpal, cpalLength] = TableRange("CPAL");
        if (colrLength < 14 || cpalLength < 12) return false;

        auto table = CreateScope<ColrLayers>();

        // Version 0's four fields sit at the front of every version, so a v1 table is read for the
        // v0 glyphs it still lists. A v1-only face reports zero of them and gets no colour here,
        // which is the honest answer: its glyphs are a paint graph, not a layer list.
        const u32 baseCount   = Be16(m_Data, colr + 2);
        const u32 baseOffset  = Be32(m_Data, colr + 4);
        const u32 layerOffset = Be32(m_Data, colr + 8);
        const u32 layerCount  = Be16(m_Data, colr + 12);
        if (baseCount == 0 || layerCount == 0) return false;

        table->bases.reserve(baseCount);
        for (u32 i = 0; i < baseCount; ++i) {
            const std::size_t at = colr + baseOffset + static_cast<std::size_t>(i) * 6;
            if (at + 6 > m_Data.size()) return false;
            table->bases.push_back({ Be16(m_Data, at), Be16(m_Data, at + 2), Be16(m_Data, at + 4) });
        }
        table->layers.reserve(layerCount);
        for (u32 i = 0; i < layerCount; ++i) {
            const std::size_t at = colr + layerOffset + static_cast<std::size_t>(i) * 4;
            if (at + 4 > m_Data.size()) return false;
            table->layers.push_back({ Be16(m_Data, at), Be16(m_Data, at + 2) });
        }
        // The spec requires the base records to be sorted so a reader can bisect them. Sorting
        // rather than trusting it costs one pass at load and makes Find's precondition true.
        std::ranges::sort(table->bases, {}, &ColrLayers::Base::glyph);

        // CPAL, palette 0. A font may ship several — light and dark variants of the same emoji —
        // and choosing between them is a document-level decision nothing in VAE makes.
        const u32 entries  = Be16(m_Data, cpal + 2);
        const u32 palettes = Be16(m_Data, cpal + 4);
        const u32 records  = Be32(m_Data, cpal + 8);
        if (entries == 0 || palettes == 0) return false;
        const u32 first = Be16(m_Data, cpal + 12);       // colorRecordIndices[0]

        table->palette.reserve(entries);
        for (u32 i = 0; i < entries; ++i) {
            const std::size_t at = cpal + records + (static_cast<std::size_t>(first) + i) * 4;
            if (at + 4 > m_Data.size()) return false;
            // BGRA in the file, which is the one place in a font where the order is not the
            // obvious one, and a silent red/blue swap if it is missed.
            table->palette.push_back({ m_Data[at + 2], m_Data[at + 1], m_Data[at], m_Data[at + 3] });
        }

        m_Colr = std::move(table);
        return true;
    }

    bool Font::Init() {
        m_Impl = CreateScope<FontImpl>();
        m_Sfnt = SfntOffset(m_Data);
        // Nothing below reads a byte until this says the directory is inside the file.
        if (!Plausible(m_Data, m_Sfnt)) {
            VAE_CORE_ERROR("'{}' is not a font: {} bytes with no readable table directory",
                           m_Name, m_Data.size());
            return false;
        }
        m_Outlines = stbtt_InitFont(&m_Impl->info, m_Data.data(), static_cast<int>(m_Sfnt));

        // Colour is asked about whether or not the outlines loaded, because two of the three
        // formats sit *on top of* an ordinary font. An sbix or COLR face is a normal face with an
        // extra table and stb reads it happily; only CBDT replaces `glyf` outright, which is why
        // stb refuses those and why the whole face has to answer through HarfBuzz instead.
        if (InitCbdt())                    m_ColourFormat = ColourFormat::Cbdt;
        else if (InitSbix())               m_ColourFormat = ColourFormat::Sbix;
        // COLR builds a glyph out of other glyphs in the same font, so without outlines there is
        // nothing for a layer to be. It is the one format that cannot stand on its own.
        else if (m_Outlines && InitColr()) m_ColourFormat = ColourFormat::Colr;

        if (!m_Outlines && m_ColourFormat == ColourFormat::None) {
            if (HasTable("SVG "))
                VAE_CORE_INFO("'{}' stores its colour glyphs as OpenType-SVG, which this build "
                              "does not read — skipping it", m_Name);
            else if (HasTable("COLR"))
                VAE_CORE_INFO("'{}' is COLR with no version-0 layer list (a v1 paint graph), which "
                              "this build does not read — skipping it", m_Name);
            else
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

        if (m_Outlines) {
            m_Family = NameString(16);              // typographic family, when present
            if (m_Family.empty()) m_Family = NameString(1);
            m_Style  = NameString(17);
            if (m_Style.empty()) m_Style = NameString(2);
        } else if (auto* font = static_cast<hb_font_t*>(ShaperFont(16.0f))) {
            // No stb font to read the name table through, so HarfBuzz reads the same table.
            char buffer[128];
            unsigned size = sizeof(buffer);
            if (hb_ot_name_get_utf8(hb_font_get_face(font), HB_OT_NAME_ID_FONT_FAMILY,
                                    HB_LANGUAGE_INVALID, &size, buffer) && size > 0)
                m_Family.assign(buffer, size);
        }
        if (m_Family.empty()) m_Family = m_Name;

        switch (m_ColourFormat) {
            case ColourFormat::Cbdt:
                VAE_CORE_INFO("'{}' is a colour font (CBDT): {} glyph ranges at {}px",
                              m_Family, m_Strike->ranges.size(), m_Strike->ppem);
                break;
            case ColourFormat::Sbix:
                VAE_CORE_INFO("'{}' is a colour font (sbix): {} glyphs at {}px",
                              m_Family, m_Sbix->glyphs, m_Sbix->ppem);
                break;
            case ColourFormat::Colr:
                VAE_CORE_INFO("'{}' is a colour font (COLR/CPAL): {} layered glyphs, {} palette "
                              "entries", m_Family, m_Colr->bases.size(), m_Colr->palette.size());
                break;
            case ColourFormat::None: break;
        }
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

    // One glyph's picture, wherever it came from. CBDT and sbix disagree about where the metrics
    // live — CBDT writes them in front of the PNG, sbix writes an origin and lets the PNG's own
    // header say how big it is — so this is what they are both reduced to.
    namespace {
        struct Picture {
            const u8*   data = nullptr;
            std::size_t length = 0;
            i32 width = 0, height = 0;        // pixels at `ppem`
            i32 bearingX = 0, bearingY = 0;   // pixels, y-up from the baseline to the top-left
            u32 ppem = 0;
        };

        Picture FindCbdt(const std::vector<u8>& data, const ColourStrike& strike, u32 glyph) {
            Picture entry;
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

                entry.data   = data.data() + begin + header;
                entry.length = end - begin - header;
                entry.ppem   = strike.ppem;
                return entry;
            }
            return entry;
        }

        Picture FindSbix(const std::vector<u8>& data, const SbixStrike& strike, u32 glyph,
                         int depth = 0) {
            Picture entry;
            if (glyph >= strike.glyphs) return entry;

            const std::size_t offsets = strike.strike + 4;
            const u32 begin = Be32(data, offsets + static_cast<std::size_t>(glyph) * 4);
            const u32 end   = Be32(data, offsets + (static_cast<std::size_t>(glyph) + 1) * 4);
            // Equal offsets mean this glyph has no picture in this strike, which is the normal
            // state of every glyph in the font that is not an emoji.
            if (end <= begin || end - begin < 8) return entry;

            const std::size_t at = strike.strike + begin;
            if (at + (end - begin) > data.size()) return entry;

            char tag[5] = {};
            std::memcpy(tag, data.data() + at + 4, 4);

            // 'dupe' is not a picture: it is a glyph id to draw instead, which is how a font ships
            // one image for several code points. Followed once — a cycle would otherwise be a hang.
            if (std::memcmp(tag, "dupe", 4) == 0) {
                if (depth > 0 || end - begin < 10) return entry;
                return FindSbix(data, strike, Be16(data, at + 8), depth + 1);
            }
            // stb decodes both of these. 'tiff' it does not, and no shipping font uses it.
            if (std::memcmp(tag, "png ", 4) != 0 && std::memcmp(tag, "jpg ", 4) != 0) return entry;

            entry.data   = data.data() + at + 8;
            entry.length = end - begin - 8;
            entry.ppem   = strike.ppem;

            // The table says where the picture goes and not how big it is, so its own header does.
            int width = 0, height = 0, channels = 0;
            if (!stbi_info_from_memory(entry.data, static_cast<int>(entry.length),
                                       &width, &height, &channels) || width <= 0 || height <= 0) {
                entry.data = nullptr;
                return entry;
            }
            entry.width  = width;
            entry.height = height;
            // originOffset is where the picture's *bottom* left goes, y-up from the baseline. The
            // top is that plus its height, which is the bearing every other glyph reports.
            entry.bearingX = BeS16(data, at);
            entry.bearingY = BeS16(data, at + 2) + height;
            return entry;
        }

        // Which of the two a face has is decided at load; only one is ever set.
        Picture FindPicture(const std::vector<u8>& data, const ColourStrike* cbdt,
                            const SbixStrike* sbix, u32 glyph) {
            if (cbdt) return FindCbdt(data, *cbdt, glyph);
            if (sbix) return FindSbix(data, *sbix, glyph);
            return {};
        }
    }


    bool Font::HasColourGlyph(u32 glyph) const {
        switch (m_ColourFormat) {
            case ColourFormat::Cbdt:
            case ColourFormat::Sbix:
                return FindPicture(m_Data, m_Strike.get(), m_Sbix.get(), glyph).data != nullptr;
            // A COLR glyph whose layers are *all* drawn in the text colour is not a colour glyph:
            // it is one shape described awkwardly, and the outline path draws it in the colour the
            // text actually asked for. See RasterizeColr.
            case ColourFormat::Colr: {
                const auto* base = m_Colr->Find(glyph);
                if (!base) return false;
                for (u32 i = 0; i < base->count; ++i) {
                    const std::size_t at = base->first + i;
                    if (at < m_Colr->layers.size()
                        && m_Colr->layers[at].palette != ColrLayers::kForeground)
                        return true;
                }
                return false;
            }
            case ColourFormat::None: return false;
        }
        return false;
    }

    // How much a picture has to shrink to be drawn at this size. They ship at one large ppem and a
    // UI asks for 14 or 16, so this is almost always a downscale.
    static f32 PictureScale(u32 ppem, f32 pixelSize) {
        return ppem > 0 ? pixelSize / static_cast<f32>(ppem) : 1.0f;
    }

    GlyphMetrics Font::PictureGlyph(u32 glyph, f32 pixelSize) const {
        GlyphMetrics m;
        const Picture entry = FindPicture(m_Data, m_Strike.get(), m_Sbix.get(), glyph);
        if (!entry.data || entry.width <= 0 || entry.height <= 0) return m;

        const f32 scale = PictureScale(entry.ppem, pixelSize);
        m.size = { static_cast<f32>(entry.width) * scale, static_cast<f32>(entry.height) * scale };
        // The bearing is y-up from the baseline to the top of the picture; everything here is
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

    GlyphBitmap Font::RasterizePicture(u32 glyph, f32 pixelSize) const {
        GlyphBitmap bitmap;
        const Picture entry = FindPicture(m_Data, m_Strike.get(), m_Sbix.get(), glyph);
        if (!entry.data || entry.length == 0) return bitmap;

        int width = 0, height = 0, channels = 0;
        u8* decoded = stbi_load_from_memory(entry.data, static_cast<int>(entry.length),
                                            &width, &height, &channels, 4);
        if (!decoded || width <= 0 || height <= 0) { stbi_image_free(decoded); return bitmap; }

        const f32 scale = PictureScale(entry.ppem, pixelSize);
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
            // STBIR_RGBA, not _PM: a PNG carries *straight* alpha, and so does the atlas. Telling
            // the resampler the data is already premultiplied makes it average the raw colour of
            // fully transparent pixels into the edge — which is black in every PNG encoder, so
            // every emoji comes out with a dark halo. STBIR_RGBA is the one that weights by alpha.
            stbir_resize_uint8_srgb(decoded, width, height, 0,
                                    bitmap.pixels.data(), outWidth, outHeight, 0,
                                    STBIR_RGBA);
        }
        stbi_image_free(decoded);
        return bitmap;
    }

    // --- COLR/CPAL ---------------------------------------------------------------------------
    //
    // The layers are composited into one RGBA picture here rather than handed to the renderer as
    // several tinted draws. Both would look identical: COLRv0 layers are flat palette colours
    // composited source-over, which is exactly what the atlas can hold. What a per-layer draw
    // would buy is choosing the palette at draw time — and nothing in VAE chooses one, because a
    // document has no palette control and CPAL's alternates are a font-authoring feature. So this
    // costs one atlas entry per glyph instead of N instances per glyph per frame, and the renderer
    // does not learn about a font format.

    // Every layer's bitmap box at this size, unioned. This is the composite's extent, and it is
    // *not* the base glyph's own box — a base glyph in a COLR font is usually empty.
    static bool ColrBounds(const stbtt_fontinfo& info, const ColrLayers& colr,
                           const ColrLayers::Base& base, f32 scale,
                           int& x0, int& y0, int& x1, int& y1) {
        bool any = false;
        for (u32 i = 0; i < base.count; ++i) {
            const std::size_t at = base.first + i;
            if (at >= colr.layers.size()) continue;
            int lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
            stbtt_GetGlyphBitmapBox(&info, static_cast<int>(colr.layers[at].glyph),
                                    scale, scale, &lx0, &ly0, &lx1, &ly1);
            if (lx1 <= lx0 || ly1 <= ly0) continue;
            if (!any) { x0 = lx0; y0 = ly0; x1 = lx1; y1 = ly1; any = true; }
            else {
                x0 = std::min(x0, lx0); y0 = std::min(y0, ly0);
                x1 = std::max(x1, lx1); y1 = std::max(y1, ly1);
            }
        }
        return any;
    }

    GlyphMetrics Font::ColrGlyph(u32 glyph, f32 pixelSize) const {
        GlyphMetrics m;
        const auto* base = m_Colr->Find(glyph);
        if (!base) return m;

        const f32 scale = Scale(pixelSize);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!ColrBounds(m_Impl->info, *m_Colr, *base, scale, x0, y0, x1, y1)) return m;

        // The advance is the base glyph's own, from hmtx: the layers are how it is drawn and say
        // nothing about how much room it takes.
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(&m_Impl->info, static_cast<int>(glyph), &advance, &bearing);

        m.advance = static_cast<f32>(advance) * scale;
        m.bearing = { static_cast<f32>(x0), static_cast<f32>(y0) };   // y0 is already y-down
        m.size    = { static_cast<f32>(x1 - x0), static_cast<f32>(y1 - y0) };
        m.blank   = false;
        return m;
    }

    GlyphBitmap Font::RasterizeColr(u32 glyph, f32 pixelSize) const {
        GlyphBitmap bitmap;
        const auto* base = m_Colr->Find(glyph);
        if (!base) return bitmap;

        const f32 scale = Scale(pixelSize);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!ColrBounds(m_Impl->info, *m_Colr, *base, scale, x0, y0, x1, y1)) return bitmap;

        const int width = x1 - x0, height = y1 - y0;
        bitmap.channels = 4;
        bitmap.width  = static_cast<u32>(width);
        bitmap.height = static_cast<u32>(height);
        bitmap.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);

        std::vector<u8> coverage;
        for (u32 i = 0; i < base->count; ++i) {
            const std::size_t at = base->first + i;
            if (at >= m_Colr->layers.size()) continue;
            const ColrLayers::Layer& layer = m_Colr->layers[at];

            // A layer drawn in the text colour cannot be: the atlas is keyed by face, glyph and
            // size, so it has no colour to bake. Black is what it becomes, and a glyph whose
            // layers are *all* foreground never reaches here — HasColourGlyph sends it down the
            // outline path, where it gets the real text colour.
            ColrLayers::Rgba colour{ 0, 0, 0, 255 };
            if (layer.palette != ColrLayers::kForeground && layer.palette < m_Colr->palette.size())
                colour = m_Colr->palette[layer.palette];
            if (colour.a == 0) continue;

            int lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
            stbtt_GetGlyphBitmapBox(&m_Impl->info, static_cast<int>(layer.glyph),
                                    scale, scale, &lx0, &ly0, &lx1, &ly1);
            const int lw = lx1 - lx0, lh = ly1 - ly0;
            if (lw <= 0 || lh <= 0) continue;

            coverage.assign(static_cast<std::size_t>(lw) * lh, 0);
            stbtt_MakeGlyphBitmap(&m_Impl->info, coverage.data(), lw, lh, lw, scale, scale,
                                  static_cast<int>(layer.glyph));

            // Source-over in premultiplied 8-bit. Compositing has to be premultiplied — that is
            // what source-over means — and the buffer is unmultiplied back to straight alpha once
            // at the end, rather than a divide per pixel per layer.
            for (int y = 0; y < lh; ++y) {
                const int destY = ly0 - y0 + y;
                if (destY < 0 || destY >= height) continue;
                for (int x = 0; x < lw; ++x) {
                    const int destX = lx0 - x0 + x;
                    if (destX < 0 || destX >= width) continue;
                    const u32 mask = coverage[static_cast<std::size_t>(y) * lw + x];
                    if (mask == 0) continue;

                    const u32 alpha = (mask * colour.a + 127) / 255;
                    if (alpha == 0) continue;
                    const u32 src[4] = { (colour.r * alpha + 127) / 255,
                                         (colour.g * alpha + 127) / 255,
                                         (colour.b * alpha + 127) / 255, alpha };
                    u8* dest = &bitmap.pixels[(static_cast<std::size_t>(destY) * width + destX) * 4];
                    for (int c = 0; c < 4; ++c)
                        dest[c] = static_cast<u8>(std::min(255u,
                                    src[c] + (dest[c] * (255 - alpha) + 127) / 255));
                }
            }
        }

        // Back to straight alpha, which is what the atlas holds and what every other rasterizer
        // here produces. A pixel nothing drew stays (0,0,0,0) and is never sampled anyway.
        for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
            const u32 alpha = bitmap.pixels[i + 3];
            if (alpha == 0 || alpha == 255) continue;
            for (int c = 0; c < 3; ++c)
                bitmap.pixels[i + c] = static_cast<u8>(
                    std::min(255u, (bitmap.pixels[i + c] * 255u + alpha / 2) / alpha));
        }
        return bitmap;
    }

    FontMetrics Font::Metrics(f32 pixelSize) const {
        // No outlines means no `hhea` that stb can reach, because stb never opened the file. The
        // same numbers come out of HarfBuzz, which did.
        if (!m_Outlines) {
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
        if (!m_Outlines) {
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
        // Per glyph, not per face. A CBDT face has a picture for everything it covers and nothing
        // else to draw; a COLR or sbix face draws most of its glyphs as ordinary outlines and only
        // some as colour, so asking the face would get every letter in it wrong.
        if (HasColourGlyph(glyph)) {
            if (m_ColourFormat == ColourFormat::Colr) return ColrGlyph(glyph, pixelSize);
            return PictureGlyph(glyph, pixelSize);
        }
        if (!m_Outlines) return {};
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
        if (!m_Outlines || !leftGlyph || !rightGlyph) return 0.0f;
        const auto a = static_cast<int>(leftGlyph);
        const auto b = static_cast<int>(rightGlyph);
        return static_cast<f32>(stbtt_GetGlyphKernAdvance(&m_Impl->info, a, b)) * Scale(pixelSize);
    }

    GlyphBitmap Font::Rasterize(u32 glyph, f32 pixelSize) const {
        if (HasColourGlyph(glyph)) {
            if (m_ColourFormat == ColourFormat::Colr) return RasterizeColr(glyph, pixelSize);
            return RasterizePicture(glyph, pixelSize);
        }
        if (!m_Outlines) return {};
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
