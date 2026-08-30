#include "vaepch.h"
#include "vae/text/ColrGraph.h"

#include "vae/vector/Raster.h"

#include <algorithm>
#include <cmath>

// COLR version 1, read and drawn.
//
// The table is a forest: a base glyph names a paint, a paint names other paints, and the leaves are
// fills. Drawing one is a depth-first walk carrying two things — the transform in force, and the
// clip established by the last PaintGlyph. Everything a fill does happens inside that clip, which
// is why a gradient never needs to know what shape it is filling.
//
// Offsets in this table come in three flavours and the difference matters: the base-glyph and layer
// lists hold Offset32s from the start of *their own list*, and every paint holds Offset24s from the
// start of *itself*. Reading one as the other lands inside a neighbouring table and draws garbage
// rather than failing, which is why every read below goes through a helper that says which it is.
namespace vae::text {

    using vector::Affine;

    namespace {

        constexpr u32 kForeground = 0xFFFFu;      // "the colour the text is", which an atlas has not
        constexpr int kMaxDepth = 24;             // the spec's own recursion limit is 64; this is
                                                  // deeper than any real font and cheap to prove

        u32 Be16(const std::vector<u8>& d, std::size_t at) {
            return at + 1 < d.size() ? (static_cast<u32>(d[at]) << 8) | d[at + 1] : 0;
        }
        i32 Sword(const std::vector<u8>& d, std::size_t at) {
            return static_cast<i16>(Be16(d, at));
        }
        u32 Be24(const std::vector<u8>& d, std::size_t at) {
            return at + 2 < d.size() ? (static_cast<u32>(d[at]) << 16)
                                     | (static_cast<u32>(d[at + 1]) << 8) | d[at + 2] : 0;
        }
        u32 Be32(const std::vector<u8>& d, std::size_t at) {
            return at + 3 < d.size() ? (static_cast<u32>(d[at]) << 24)
                                     | (static_cast<u32>(d[at + 1]) << 16)
                                     | (static_cast<u32>(d[at + 2]) << 8) | d[at + 3] : 0;
        }
        // F2DOT14: a signed 2.14 fixed-point number. Used for scales, alphas and — multiplied by
        // 180 — every angle in the table.
        f32 F2Dot14(const std::vector<u8>& d, std::size_t at) {
            return static_cast<f32>(static_cast<i16>(Be16(d, at))) / 16384.0f;
        }
        // Fixed: signed 16.16, which only the general Affine2x3 uses.
        f32 Fixed(const std::vector<u8>& d, std::size_t at) {
            return static_cast<f32>(static_cast<i32>(Be32(d, at))) / 65536.0f;
        }

        bool Invert(const Affine& m, Affine& out) {
            const f32 determinant = m.a * m.d - m.b * m.c;
            if (std::abs(determinant) < 1.0e-9f) return false;
            const f32 inv = 1.0f / determinant;
            out.a =  m.d * inv;
            out.b = -m.b * inv;
            out.c = -m.c * inv;
            out.d =  m.a * inv;
            out.e = (m.c * m.f - m.d * m.e) * inv;
            out.f = (m.b * m.e - m.a * m.f) * inv;
            return true;
        }

        // --- the canvas ---------------------------------------------------------------------
        //
        // Premultiplied 8-bit RGBA, because that is what source-over means; the picture is
        // unmultiplied once at the very end rather than per pixel per layer.
        struct Canvas {
            u32 width = 0, height = 0;
            std::vector<u8> px;

            void Reset(u32 w, u32 h) {
                width = w; height = h;
                px.assign(static_cast<std::size_t>(w) * h * 4, 0);
            }
            std::size_t At(u32 x, u32 y) const {
                return (static_cast<std::size_t>(y) * width + x) * 4;
            }
        };

        struct Colour { f32 r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f; };   // straight, 0..1

        // What a paint is drawn through: the transform in force and the clip the enclosing
        // PaintGlyph established. An empty clip means "everything", which only happens above the
        // first PaintGlyph — where nothing draws anyway.
        struct State {
            Affine transform;
            std::vector<u8> clip;
        };

        // --- colour lines --------------------------------------------------------------------

        enum class Extend : u8 { Pad = 0, Repeat = 1, Reflect = 2 };

        struct Stop { f32 at = 0.0f; u32 palette = 0; f32 alpha = 1.0f; };

        struct ColourLine {
            Extend extend = Extend::Pad;
            std::vector<Stop> stops;
        };

        // --- compositing ----------------------------------------------------------------------

        enum : u8 {
            kClear = 0, kSrc = 1, kDest = 2, kSrcOver = 3, kDestOver = 4, kSrcIn = 5, kDestIn = 6,
            kSrcOut = 7, kDestOut = 8, kSrcAtop = 9, kDestAtop = 10, kXor = 11, kPlus = 12,
            kScreen = 13, kOverlay = 14, kDarken = 15, kLighten = 16, kColorDodge = 17,
            kColorBurn = 18, kHardLight = 19, kSoftLight = 20, kDifference = 21, kExclusion = 22,
            kMultiply = 23, kHslHue = 24, kHslSaturation = 25, kHslColor = 26, kHslLuminosity = 27,
        };

        f32 BlendChannel(u8 mode, f32 cs, f32 cb) {
            switch (mode) {
                case kScreen:     return cs + cb - cs * cb;
                case kOverlay:    return cb <= 0.5f ? 2.0f * cs * cb
                                                    : 1.0f - 2.0f * (1.0f - cs) * (1.0f - cb);
                case kDarken:     return std::min(cs, cb);
                case kLighten:    return std::max(cs, cb);
                case kColorDodge: return cb <= 0.0f ? 0.0f
                                       : (cs >= 1.0f ? 1.0f : std::min(1.0f, cb / (1.0f - cs)));
                case kColorBurn:  return cb >= 1.0f ? 1.0f
                                       : (cs <= 0.0f ? 0.0f
                                                     : 1.0f - std::min(1.0f, (1.0f - cb) / cs));
                case kHardLight:  return cs <= 0.5f ? 2.0f * cs * cb
                                                    : 1.0f - 2.0f * (1.0f - cs) * (1.0f - cb);
                case kSoftLight: {
                    if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
                    const f32 d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb
                                              : std::sqrt(std::max(cb, 0.0f));
                    return cb + (2.0f * cs - 1.0f) * (d - cb);
                }
                case kDifference: return std::abs(cs - cb);
                case kExclusion:  return cs + cb - 2.0f * cs * cb;
                case kMultiply:   return cs * cb;
                default:          return cs;
            }
        }

        // The non-separable modes, which work on the colour as a whole rather than channel by
        // channel. Straight out of the compositing spec — there is no shorter way to say them.
        f32 Luminosity(const Colour& c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

        Colour ClipColour(Colour c) {
            const f32 l = Luminosity(c);
            const f32 lo = std::min({ c.r, c.g, c.b });
            const f32 hi = std::max({ c.r, c.g, c.b });
            const auto scale = [&](f32 x, f32 factor, f32 target) {
                return l + (x - l) * factor / target;
            };
            if (lo < 0.0f && l - lo > 1.0e-6f) {
                c = { scale(c.r, l, l - lo), scale(c.g, l, l - lo), scale(c.b, l, l - lo), c.a };
            }
            if (hi > 1.0f && hi - l > 1.0e-6f) {
                c = { l + (c.r - l) * (1.0f - l) / (hi - l), l + (c.g - l) * (1.0f - l) / (hi - l),
                      l + (c.b - l) * (1.0f - l) / (hi - l), c.a };
            }
            return c;
        }

        Colour SetLuminosity(Colour c, f32 l) {
            const f32 delta = l - Luminosity(c);
            c.r += delta; c.g += delta; c.b += delta;
            return ClipColour(c);
        }

        f32 Saturation(const Colour& c) {
            return std::max({ c.r, c.g, c.b }) - std::min({ c.r, c.g, c.b });
        }

        Colour SetSaturation(Colour c, f32 s) {
            f32* channels[3] = { &c.r, &c.g, &c.b };
            std::sort(channels, channels + 3, [](const f32* a, const f32* b) { return *a < *b; });
            if (*channels[2] > *channels[0]) {
                *channels[1] = (*channels[1] - *channels[0]) * s / (*channels[2] - *channels[0]);
                *channels[2] = s;
            } else {
                *channels[1] = *channels[2] = 0.0f;
            }
            *channels[0] = 0.0f;
            return c;
        }

        Colour BlendNonSeparable(u8 mode, const Colour& cs, const Colour& cb) {
            switch (mode) {
                case kHslHue:
                    return SetLuminosity(SetSaturation(cs, Saturation(cb)), Luminosity(cb));
                case kHslSaturation:
                    return SetLuminosity(SetSaturation(cb, Saturation(cs)), Luminosity(cb));
                case kHslColor:        return SetLuminosity(cs, Luminosity(cb));
                case kHslLuminosity:   return SetLuminosity(cb, Luminosity(cs));
                default:               return cs;
            }
        }

        // Porter-Duff coefficients: how much of the source and of the backdrop survives.
        void PorterDuff(u8 mode, f32 as, f32 ab, f32& fs, f32& fb) {
            switch (mode) {
                case kClear:    fs = 0.0f;        fb = 0.0f;        return;
                case kSrc:      fs = 1.0f;        fb = 0.0f;        return;
                case kDest:     fs = 0.0f;        fb = 1.0f;        return;
                case kSrcOver:  fs = 1.0f;        fb = 1.0f - as;   return;
                case kDestOver: fs = 1.0f - ab;   fb = 1.0f;        return;
                case kSrcIn:    fs = ab;          fb = 0.0f;        return;
                case kDestIn:   fs = 0.0f;        fb = as;          return;
                case kSrcOut:   fs = 1.0f - ab;   fb = 0.0f;        return;
                case kDestOut:  fs = 0.0f;        fb = 1.0f - as;   return;
                case kSrcAtop:  fs = ab;          fb = 1.0f - as;   return;
                case kDestAtop: fs = 1.0f - ab;   fb = as;          return;
                case kXor:      fs = 1.0f - ab;   fb = 1.0f - as;   return;
                case kPlus:     fs = 1.0f;        fb = 1.0f;        return;
                default:        fs = 1.0f;        fb = 1.0f - as;   return;   // blends are over
            }
        }

    }

    // ------------------------------------------------------------------------------- the table

    struct ColrGraph::Impl {
        const std::vector<u8>* file = nullptr;
        u32 colr = 0, length = 0;
        u32 baseList = 0, layerList = 0, clipList = 0;
        u32 layerCount = 0;

        struct Base { u32 glyph = 0; u32 paint = 0; };    // paint is absolute in the file
        std::vector<Base> bases;

        struct ClipBox { u32 first = 0, last = 0; Rect box; };
        std::vector<ClipBox> clips;

        const Base* Find(u32 glyph) const {
            const auto it = std::lower_bound(bases.begin(), bases.end(), glyph,
                                             [](const Base& b, u32 g) { return b.glyph < g; });
            return it != bases.end() && it->glyph == glyph ? &*it : nullptr;
        }

        const ClipBox* Clip(u32 glyph) const {
            for (const ClipBox& box : clips)
                if (glyph >= box.first && glyph <= box.last) return &box;
            return nullptr;
        }

        // A paint's own offset, and whether it is inside the table at all. Every jump goes through
        // this: an offset that leaves the table is a font that would otherwise be read as whatever
        // happens to follow it in the file.
        bool Inside(u32 offset, u32 need) const {
            return offset >= colr && offset + need <= colr + length && offset + need <= file->size();
        }

        u32 SubPaint(u32 paint, std::size_t at) const {
            const u32 delta = Be24(*file, at);
            if (delta == 0) return 0;
            const u32 target = paint + delta;
            return Inside(target, 1) ? target : 0;
        }

        u32 LayerAt(u32 index) const {
            if (!layerList || index >= layerCount) return 0;
            const u32 delta = Be32(*file, layerList + 4 + static_cast<std::size_t>(index) * 4);
            const u32 target = layerList + delta;
            return delta && Inside(target, 1) ? target : 0;
        }

        ColourLine ReadColourLine(u32 at, bool variable) const {
            ColourLine line;
            if (!Inside(at, 3)) return line;
            const u32 extend = (*file)[at];
            line.extend = extend <= 2 ? static_cast<Extend>(extend) : Extend::Pad;
            const u32 count = Be16(*file, at + 1);
            const u32 stride = variable ? 10u : 6u;
            if (!Inside(at, 3 + count * stride)) return line;
            line.stops.reserve(count);
            for (u32 i = 0; i < count; ++i) {
                const std::size_t stop = at + 3 + static_cast<std::size_t>(i) * stride;
                line.stops.push_back({ F2Dot14(*file, stop), Be16(*file, stop + 2),
                                       F2Dot14(*file, stop + 4) });
            }
            // The stops have to be in order for a lookup to bisect them, and a font is allowed to
            // be wrong about that in a way no reader should inherit.
            std::stable_sort(line.stops.begin(), line.stops.end(),
                             [](const Stop& a, const Stop& b) { return a.at < b.at; });
            return line;
        }
    };

    ColrGraph::ColrGraph() : m_Impl(CreateScope<Impl>()) {}
    ColrGraph::~ColrGraph() = default;

    Scope<ColrGraph> ColrGraph::Parse(const std::vector<u8>& file, u32 colr, u32 length) {
        // Version 1's header is version 0's plus five offsets. A v0 table stops at 14 bytes and is
        // read by Font.cpp instead.
        if (length < 34 || colr + 34 > file.size()) return nullptr;
        if (Be16(file, colr) != 1) return nullptr;

        auto graph = CreateScope<ColrGraph>();
        Impl& impl = *graph->m_Impl;
        impl.file = &file;
        impl.colr = colr;
        impl.length = std::min<u32>(length, static_cast<u32>(file.size() - colr));

        const u32 baseOffset  = Be32(file, colr + 14);
        const u32 layerOffset = Be32(file, colr + 18);
        const u32 clipOffset  = Be32(file, colr + 22);
        if (!baseOffset) return nullptr;                  // v1 header, no v1 glyphs

        impl.baseList = colr + baseOffset;
        if (!impl.Inside(impl.baseList, 4)) return nullptr;
        const u32 count = Be32(file, impl.baseList);
        if (!count || !impl.Inside(impl.baseList, 4 + count * 6)) return nullptr;

        impl.bases.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            const std::size_t at = impl.baseList + 4 + static_cast<std::size_t>(i) * 6;
            const u32 paint = impl.baseList + Be32(file, at + 2);
            if (!impl.Inside(paint, 1)) continue;
            impl.bases.push_back({ Be16(file, at), paint });
        }
        if (impl.bases.empty()) return nullptr;
        std::ranges::sort(impl.bases, {}, &Impl::Base::glyph);

        if (layerOffset) {
            impl.layerList = colr + layerOffset;
            if (impl.Inside(impl.layerList, 4)) {
                impl.layerCount = Be32(file, impl.layerList);
                if (!impl.Inside(impl.layerList, 4 + impl.layerCount * 4)) {
                    impl.layerList = 0;
                    impl.layerCount = 0;
                }
            }
        }

        // The clip list is the font telling us how big each glyph draws. Believing it saves
        // walking the whole graph twice, and a font that ships one has been measured by its
        // compiler rather than by us.
        if (clipOffset) {
            const u32 list = colr + clipOffset;
            if (impl.Inside(list, 5) && (file)[list] == 1) {
                const u32 clips = Be32(file, list + 1);
                for (u32 i = 0; i < clips; ++i) {
                    const std::size_t at = list + 5 + static_cast<std::size_t>(i) * 7;
                    if (!impl.Inside(static_cast<u32>(at), 7)) break;
                    const u32 box = list + Be24(file, at + 4);
                    if (!impl.Inside(box, 9) || (file)[box] < 1 || (file)[box] > 2) continue;
                    Impl::ClipBox record;
                    record.first = Be16(file, at);
                    record.last  = Be16(file, at + 2);
                    const f32 xMin = static_cast<f32>(Sword(file, box + 1));
                    const f32 yMin = static_cast<f32>(Sword(file, box + 3));
                    const f32 xMax = static_cast<f32>(Sword(file, box + 5));
                    const f32 yMax = static_cast<f32>(Sword(file, box + 7));
                    record.box = { { xMin, yMin }, { xMax - xMin, yMax - yMin } };
                    impl.clips.push_back(record);
                }
            }
        }
        return graph;
    }

    bool ColrGraph::Has(u32 glyph) const { return m_Impl->Find(glyph) != nullptr; }

    // ------------------------------------------------------------------------------- walking

    // The walks below share their structure — dispatch on the format byte, compose transforms,
    // recurse — and are deliberately not one function with a flag. Measuring has no canvas and
    // drawing has no bounds to grow, and merging them would mean every case carrying both.
    struct ColrGraphWalker {
        const ColrGraph::Impl& table;
        const ColrGraph::Outline& outline;
        const std::vector<ColrGraph::Rgba>* palette = nullptr;
        Canvas* canvas = nullptr;
        // What a foreground colour becomes, and whether anything else was ever used.
        bool sawColour = false;

        const std::vector<u8>& File() const { return *table.file; }

        // --- geometry ------------------------------------------------------------------------

        bool GlyphPath(u32 glyph, vector::Path& path) const { return outline(glyph, path); }

        // --- measuring -------------------------------------------------------------------------

        void Grow(Rect& box, const Rect& add) const {
            if (add.size.x <= 0.0f || add.size.y <= 0.0f) return;
            if (box.size.x <= 0.0f || box.size.y <= 0.0f) { box = add; return; }
            const f32 x0 = std::min(box.pos.x, add.pos.x);
            const f32 y0 = std::min(box.pos.y, add.pos.y);
            const f32 x1 = std::max(box.pos.x + box.size.x, add.pos.x + add.size.x);
            const f32 y1 = std::max(box.pos.y + box.size.y, add.pos.y + add.size.y);
            box = { { x0, y0 }, { x1 - x0, y1 - y0 } };
        }

        Rect Transformed(const Rect& box, const Affine& m) const {
            const Vec2 corners[4] = {
                m.Apply({ box.pos.x, box.pos.y }),
                m.Apply({ box.pos.x + box.size.x, box.pos.y }),
                m.Apply({ box.pos.x, box.pos.y + box.size.y }),
                m.Apply({ box.pos.x + box.size.x, box.pos.y + box.size.y }),
            };
            f32 x0 = corners[0].x, y0 = corners[0].y, x1 = corners[0].x, y1 = corners[0].y;
            for (const Vec2& corner : corners) {
                x0 = std::min(x0, corner.x); y0 = std::min(y0, corner.y);
                x1 = std::max(x1, corner.x); y1 = std::max(y1, corner.y);
            }
            return { { x0, y0 }, { x1 - x0, y1 - y0 } };
        }

        // Which palette entries a glyph's graph actually names. Only the answer "nothing but the
        // text colour" matters — that glyph has to be drawn as an outline instead, because the
        // atlas has no colour to bake into it.
        void Palettes(u32 paint, int depth) {
            if (!paint || depth > kMaxDepth || sawColour || !table.Inside(paint, 1)) return;
            const u8 format = File()[paint];
            switch (format) {
                case 1: {                                            // PaintColrLayers
                    const u32 count = table.Inside(paint, 6) ? File()[paint + 1] : 0;
                    const u32 first = Be32(File(), paint + 2);
                    for (u32 i = 0; i < count; ++i) Palettes(table.LayerAt(first + i), depth + 1);
                    return;
                }
                case 2: case 3:                                      // PaintSolid
                    if (table.Inside(paint, 5) && Be16(File(), paint + 1) != kForeground)
                        sawColour = true;
                    return;
                case 4: case 5: case 6: case 7: case 8: case 9: {    // the three gradients
                    const bool variable = (format & 1) != 0;
                    const ColourLine line = table.ReadColourLine(table.SubPaint(paint, paint + 1),
                                                                 variable);
                    for (const Stop& stop : line.stops)
                        if (stop.palette != kForeground) sawColour = true;
                    return;
                }
                case 10:                                             // PaintGlyph
                    Palettes(table.SubPaint(paint, paint + 1), depth + 1);
                    return;
                case 11:                                             // PaintColrGlyph
                    if (const ColrGraph::Impl::Base* base = table.Find(Be16(File(), paint + 1)))
                        Palettes(base->paint, depth + 1);
                    return;
                case 32:                                             // PaintComposite
                    Palettes(table.SubPaint(paint, paint + 1), depth + 1);
                    Palettes(table.SubPaint(paint, paint + 5), depth + 1);
                    return;
                default: break;
            }
            Affine ignored;
            Palettes(Child(paint, format, ignored), depth + 1);
        }

        void Measure(u32 paint, const Affine& transform, Rect& box, int depth) {
            if (!paint || depth > kMaxDepth || !table.Inside(paint, 1)) return;
            const u8 format = File()[paint];
            switch (format) {
                case 1: {                                            // PaintColrLayers
                    const u32 count = table.Inside(paint, 6) ? File()[paint + 1] : 0;
                    const u32 first = Be32(File(), paint + 2);
                    for (u32 i = 0; i < count; ++i)
                        Measure(table.LayerAt(first + i), transform, box, depth + 1);
                    return;
                }
                case 10: {                                           // PaintGlyph
                    const u32 glyph = Be16(File(), paint + 4);
                    vector::Path path;
                    if (GlyphPath(glyph, path) && !path.Empty())
                        Grow(box, Transformed(path.ControlBounds(), transform));
                    return;
                }
                case 11: {                                           // PaintColrGlyph
                    const u32 glyph = Be16(File(), paint + 1);
                    if (const ColrGraph::Impl::Base* base = table.Find(glyph))
                        Measure(base->paint, transform, box, depth + 1);
                    return;
                }
                case 32: {                                           // PaintComposite
                    Measure(table.SubPaint(paint, paint + 1), transform, box, depth + 1);
                    Measure(table.SubPaint(paint, paint + 5), transform, box, depth + 1);
                    return;
                }
                default: break;
            }
            // Everything else either transforms one child or is a fill, and a fill has no size of
            // its own: it paints inside whatever glyph clipped it.
            Affine child = transform;
            const u32 sub = Child(paint, format, child);
            if (sub) Measure(sub, child, box, depth + 1);
        }

        // The sub-paint of a transform node, with `transform` composed onto the way in. Returns 0
        // for anything that is not a one-child node.
        u32 Child(u32 paint, u8 format, Affine& transform) const {
            const auto compose = [&](const Affine& m) { transform = m.Then(transform); };
            switch (format) {
                case 12: case 13: {                                  // PaintTransform
                    const u32 sub = table.SubPaint(paint, paint + 1);
                    const u32 matrix = table.SubPaint(paint, paint + 4);
                    if (!matrix || !table.Inside(matrix, 24)) return sub;
                    compose({ Fixed(File(), matrix),      Fixed(File(), matrix + 4),
                              Fixed(File(), matrix + 8),  Fixed(File(), matrix + 12),
                              Fixed(File(), matrix + 16), Fixed(File(), matrix + 20) });
                    return sub;
                }
                case 14: case 15: {                                  // PaintTranslate
                    compose(Affine::Translate({ static_cast<f32>(Sword(File(), paint + 4)),
                                                static_cast<f32>(Sword(File(), paint + 6)) }));
                    return table.SubPaint(paint, paint + 1);
                }
                case 16: case 17:                                    // PaintScale
                case 18: case 19:                                    // ...AroundCenter
                case 20: case 21:                                    // PaintScaleUniform
                case 22: case 23: {                                  // ...AroundCenter
                    const bool uniform = format >= 20;
                    const bool around  = format == 18 || format == 19 || format == 22
                                      || format == 23;
                    const f32 sx = F2Dot14(File(), paint + 4);
                    const f32 sy = uniform ? sx : F2Dot14(File(), paint + 6);
                    Vec2 centre{ 0.0f, 0.0f };
                    if (around) {
                        const std::size_t at = paint + (uniform ? 6 : 8);
                        centre = { static_cast<f32>(Sword(File(), at)),
                                   static_cast<f32>(Sword(File(), at + 2)) };
                    }
                    compose(Affine::Translate({ -centre.x, -centre.y })
                                .Then(Affine::Scaling({ sx, sy }))
                                .Then(Affine::Translate(centre)));
                    return table.SubPaint(paint, paint + 1);
                }
                case 24: case 25:                                    // PaintRotate
                case 26: case 27: {                                  // ...AroundCenter
                    // Angles are stored in half-turns: F2DOT14 times 180 is the number of degrees,
                    // counter-clockwise, in the font's own y-up space.
                    const f32 degrees = F2Dot14(File(), paint + 4) * 180.0f;
                    Vec2 centre{ 0.0f, 0.0f };
                    if (format >= 26)
                        centre = { static_cast<f32>(Sword(File(), paint + 6)),
                                   static_cast<f32>(Sword(File(), paint + 8)) };
                    compose(Affine::Translate({ -centre.x, -centre.y })
                                .Then(Affine::Rotate(degrees))
                                .Then(Affine::Translate(centre)));
                    return table.SubPaint(paint, paint + 1);
                }
                case 28: case 29:                                    // PaintSkew
                case 30: case 31: {                                  // ...AroundCenter
                    const f32 x = F2Dot14(File(), paint + 4) * 180.0f;
                    const f32 y = F2Dot14(File(), paint + 6) * 180.0f;
                    Vec2 centre{ 0.0f, 0.0f };
                    if (format >= 30)
                        centre = { static_cast<f32>(Sword(File(), paint + 8)),
                                   static_cast<f32>(Sword(File(), paint + 10)) };
                    // x skews *against* the angle and y with it, which is what makes both read as
                    // counter-clockwise in a y-up space.
                    const Affine skew{ 1.0f, std::tan(y * 3.14159265f / 180.0f),
                                       -std::tan(x * 3.14159265f / 180.0f), 1.0f, 0.0f, 0.0f };
                    compose(Affine::Translate({ -centre.x, -centre.y })
                                .Then(skew)
                                .Then(Affine::Translate(centre)));
                    return table.SubPaint(paint, paint + 1);
                }
                default: return 0;
            }
        }

        // --- colour ----------------------------------------------------------------------------

        Colour Look(u32 index, f32 alpha) {
            Colour colour{ 0.0f, 0.0f, 0.0f, alpha };
            if (index == kForeground) {
                // Black, and said so: the atlas is keyed by face, glyph and size, so there is no
                // text colour to bake in. A glyph made *only* of foreground never reaches here —
                // ForegroundOnly sends it down the outline path, where the real colour applies.
                return colour;
            }
            sawColour = true;
            if (!palette || index >= palette->size()) return { 0.0f, 0.0f, 0.0f, 0.0f };
            const ColrGraph::Rgba& entry = (*palette)[index];
            return { entry.r / 255.0f, entry.g / 255.0f, entry.b / 255.0f,
                     entry.a / 255.0f * alpha };
        }

        Colour Sample(const ColourLine& line, f32 t) {
            if (line.stops.empty()) return {};
            const f32 first = line.stops.front().at, last = line.stops.back().at;
            const f32 span = last - first;
            if (span <= 0.0f) {
                // Every stop in the same place: the line is one colour, and which one depends on
                // which side of it we are, exactly as a zero-width gradient behaves.
                return Look(t < first ? line.stops.front().palette : line.stops.back().palette,
                            t < first ? line.stops.front().alpha : line.stops.back().alpha);
            }
            switch (line.extend) {
                case Extend::Repeat: {
                    const f32 turns = (t - first) / span;
                    t = first + (turns - std::floor(turns)) * span;
                    break;
                }
                case Extend::Reflect: {
                    const f32 turns = (t - first) / span;
                    f32 phase = std::fmod(std::abs(turns), 2.0f);
                    if (phase > 1.0f) phase = 2.0f - phase;
                    t = first + phase * span;
                    break;
                }
                case Extend::Pad:
                default: t = std::clamp(t, first, last); break;
            }
            for (std::size_t i = 1; i < line.stops.size(); ++i) {
                const Stop& a = line.stops[i - 1];
                const Stop& b = line.stops[i];
                if (t > b.at) continue;
                const f32 width = b.at - a.at;
                const f32 k = width > 0.0f ? std::clamp((t - a.at) / width, 0.0f, 1.0f) : 0.0f;
                const Colour ca = Look(a.palette, a.alpha);
                const Colour cb = Look(b.palette, b.alpha);
                return { ca.r + (cb.r - ca.r) * k, ca.g + (cb.g - ca.g) * k,
                         ca.b + (cb.b - ca.b) * k, ca.a + (cb.a - ca.a) * k };
            }
            return Look(line.stops.back().palette, line.stops.back().alpha);
        }

        // --- drawing ---------------------------------------------------------------------------

        // One pixel of a fill, composited source-over into the canvas through the clip.
        void Put(Canvas& target, std::size_t at, const Colour& colour, u32 coverage) {
            const f32 alpha = colour.a * static_cast<f32>(coverage) / 255.0f;
            if (alpha <= 0.0f) return;
            const auto blend = [&](u8 dest, f32 src) {
                const f32 out = src * alpha + static_cast<f32>(dest) / 255.0f * (1.0f - alpha);
                return static_cast<u8>(std::clamp(out * 255.0f + 0.5f, 0.0f, 255.0f));
            };
            target.px[at]     = blend(target.px[at],     colour.r);
            target.px[at + 1] = blend(target.px[at + 1], colour.g);
            target.px[at + 2] = blend(target.px[at + 2], colour.b);
            target.px[at + 3] = blend(target.px[at + 3], 1.0f);
        }

        void FillSolid(Canvas& target, const std::vector<u8>& clip, const Colour& colour) {
            for (u32 y = 0; y < target.height; ++y)
                for (u32 x = 0; x < target.width; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * target.width + x;
                    const u32 coverage = clip.empty() ? 255u : clip[i];
                    if (coverage) Put(target, i * 4, colour, coverage);
                }
        }

        // A gradient is defined in the paint's own coordinate space, so every pixel is mapped back
        // through the inverse of the transform in force rather than the geometry being mapped
        // forward. That is also what makes a rotated gradient come out rotated rather than skewed.
        template <typename Parameter>
        void FillGradient(Canvas& target, const std::vector<u8>& clip, const ColourLine& line,
                          const Affine& toPaint, Parameter parameter) {
            for (u32 y = 0; y < target.height; ++y)
                for (u32 x = 0; x < target.width; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * target.width + x;
                    const u32 coverage = clip.empty() ? 255u : clip[i];
                    if (!coverage) continue;
                    // The centre of the pixel, which is where its colour is decided.
                    const Vec2 point = toPaint.Apply({ static_cast<f32>(x) + 0.5f,
                                                       static_cast<f32>(y) + 0.5f });
                    f32 t = 0.0f;
                    if (!parameter(point, t)) continue;
                    Put(target, i * 4, Sample(line, t), coverage);
                }
        }

        void Draw(u32 paint, Canvas& target, const State& state, int depth);

        // The clip a PaintGlyph establishes: the glyph's coverage, narrowed by whatever was
        // already clipping.
        std::vector<u8> ClipFor(u32 glyph, const State& state, u32 width, u32 height) {
            vector::Path path;
            if (!GlyphPath(glyph, path) || path.Empty()) return {};
            const auto contours = path.Flatten(state.transform, 0.2f);
            const vector::Mask mask = vector::Fill(contours, vector::FillRule::NonZero,
                                                   width, height);
            if (mask.Empty()) return {};
            std::vector<u8> clip = mask.coverage;
            if (!state.clip.empty() && state.clip.size() == clip.size())
                for (std::size_t i = 0; i < clip.size(); ++i)
                    clip[i] = static_cast<u8>((clip[i] * state.clip[i] + 127) / 255);
            return clip;
        }
    };

    void ColrGraphWalker::Draw(u32 paint, Canvas& target, const State& state, int depth) {
        if (!paint || depth > kMaxDepth || !table.Inside(paint, 1)) return;
        const u8 format = File()[paint];

        switch (format) {
            case 1: {                                                // PaintColrLayers
                if (!table.Inside(paint, 6)) return;
                const u32 count = File()[paint + 1];
                const u32 first = Be32(File(), paint + 2);
                // First is bottom-most: the list is painted in order.
                for (u32 i = 0; i < count; ++i)
                    Draw(table.LayerAt(first + i), target, state, depth + 1);
                return;
            }

            case 2: case 3: {                                        // PaintSolid
                if (!table.Inside(paint, 5)) return;
                FillSolid(target, state.clip,
                          Look(Be16(File(), paint + 1), F2Dot14(File(), paint + 3)));
                return;
            }

            case 4: case 5: {                                        // PaintLinearGradient
                if (!table.Inside(paint, 16)) return;
                const ColourLine line = table.ReadColourLine(table.SubPaint(paint, paint + 1),
                                                             format == 5);
                if (line.stops.empty()) return;
                const Vec2 p0{ static_cast<f32>(Sword(File(), paint + 4)),
                               static_cast<f32>(Sword(File(), paint + 6)) };
                const Vec2 p1{ static_cast<f32>(Sword(File(), paint + 8)),
                               static_cast<f32>(Sword(File(), paint + 10)) };
                const Vec2 p2{ static_cast<f32>(Sword(File(), paint + 12)),
                               static_cast<f32>(Sword(File(), paint + 14)) };
                // p2 is a rotation point, not an end point: the gradient runs along p0->p1 after
                // p1 has been projected onto the line perpendicular to p0->p2. That is what lets a
                // font express a gradient at an angle to the line between its two ends.
                const Vec2 v{ p1.x - p0.x, p1.y - p0.y };
                const Vec2 u{ p2.x - p0.x, p2.y - p0.y };
                const f32 uu = u.x * u.x + u.y * u.y;
                Vec2 axis = v;
                if (uu > 0.0f) {
                    const f32 k = (v.x * u.x + v.y * u.y) / uu;
                    axis = { v.x - u.x * k, v.y - u.y * k };
                }
                const f32 span = axis.x * axis.x + axis.y * axis.y;
                if (span <= 0.0f) return;
                Affine toPaint;
                if (!Invert(state.transform, toPaint)) return;
                FillGradient(target, state.clip, line, toPaint,
                             [&](const Vec2& point, f32& t) {
                                 t = ((point.x - p0.x) * axis.x + (point.y - p0.y) * axis.y) / span;
                                 return true;
                             });
                return;
            }

            case 6: case 7: {                                        // PaintRadialGradient
                if (!table.Inside(paint, 16)) return;
                const ColourLine line = table.ReadColourLine(table.SubPaint(paint, paint + 1),
                                                             format == 7);
                if (line.stops.empty()) return;
                const Vec2 c0{ static_cast<f32>(Sword(File(), paint + 4)),
                               static_cast<f32>(Sword(File(), paint + 6)) };
                const f32  r0 = static_cast<f32>(Sword(File(), paint + 8));
                const Vec2 c1{ static_cast<f32>(Sword(File(), paint + 10)),
                               static_cast<f32>(Sword(File(), paint + 12)) };
                const f32  r1 = static_cast<f32>(Sword(File(), paint + 14));
                Affine toPaint;
                if (!Invert(state.transform, toPaint)) return;

                // Two circles interpolated: find the largest t whose circle passes through the
                // point and whose radius is not negative. This is the same two-point conical
                // gradient a browser draws, and the quadratic is where all of its behaviour —
                // cones, the "no colour here" region — comes from.
                const f32 cdx = c1.x - c0.x, cdy = c1.y - c0.y, dr = r1 - r0;
                const f32 a = cdx * cdx + cdy * cdy - dr * dr;
                FillGradient(target, state.clip, line, toPaint,
                             [&](const Vec2& point, f32& t) {
                                 const f32 px = point.x - c0.x, py = point.y - c0.y;
                                 const f32 b = px * cdx + py * cdy + r0 * dr;
                                 const f32 c = px * px + py * py - r0 * r0;
                                 if (std::abs(a) < 1.0e-6f) {
                                     if (std::abs(b) < 1.0e-9f) return false;
                                     t = c / (2.0f * b);
                                     return r0 + t * dr >= 0.0f;
                                 }
                                 const f32 disc = b * b - a * c;
                                 if (disc < 0.0f) return false;
                                 const f32 root = std::sqrt(disc);
                                 const f32 hi = (b + root) / a, lo = (b - root) / a;
                                 if (r0 + std::max(hi, lo) * dr >= 0.0f) {
                                     t = std::max(hi, lo);
                                     return true;
                                 }
                                 t = std::min(hi, lo);
                                 return r0 + t * dr >= 0.0f;
                             });
                return;
            }

            case 8: case 9: {                                        // PaintSweepGradient
                if (!table.Inside(paint, 12)) return;
                const ColourLine line = table.ReadColourLine(table.SubPaint(paint, paint + 1),
                                                             format == 9);
                if (line.stops.empty()) return;
                const Vec2 centre{ static_cast<f32>(Sword(File(), paint + 4)),
                                   static_cast<f32>(Sword(File(), paint + 6)) };
                const f32 start = F2Dot14(File(), paint + 8) * 180.0f;
                const f32 end   = F2Dot14(File(), paint + 10) * 180.0f;
                if (std::abs(end - start) < 1.0e-4f) return;
                Affine toPaint;
                if (!Invert(state.transform, toPaint)) return;
                FillGradient(target, state.clip, line, toPaint,
                             [&](const Vec2& point, f32& t) {
                                 f32 angle = std::atan2(point.y - centre.y, point.x - centre.x)
                                           * 180.0f / 3.14159265f;
                                 if (angle < 0.0f) angle += 360.0f;
                                 t = (angle - start) / (end - start);
                                 return true;
                             });
                return;
            }

            case 10: {                                               // PaintGlyph
                if (!table.Inside(paint, 6)) return;
                const u32 glyph = Be16(File(), paint + 4);
                State inner;
                inner.transform = state.transform;
                inner.clip = ClipFor(glyph, state, target.width, target.height);
                if (inner.clip.empty()) return;                      // clipped to nothing
                Draw(table.SubPaint(paint, paint + 1), target, inner, depth + 1);
                return;
            }

            case 11: {                                               // PaintColrGlyph
                if (!table.Inside(paint, 3)) return;
                if (const ColrGraph::Impl::Base* base = table.Find(Be16(File(), paint + 1)))
                    Draw(base->paint, target, state, depth + 1);
                return;
            }

            case 32: {                                               // PaintComposite
                if (!table.Inside(paint, 8)) return;
                const u32 source   = table.SubPaint(paint, paint + 1);
                const u8  mode     = File()[paint + 4];
                const u32 backdrop = table.SubPaint(paint, paint + 5);

                // Both halves are drawn into groups of their own and combined, rather than one
                // onto the other in place: every mode but source-over needs to see the backdrop's
                // alpha where the source is *not*, and painting in place has already lost it.
                Canvas below, above;
                below.Reset(target.width, target.height);
                above.Reset(target.width, target.height);
                Draw(backdrop, below, state, depth + 1);
                Draw(source, above, state, depth + 1);

                for (std::size_t i = 0; i + 3 < below.px.size(); i += 4) {
                    const f32 as = above.px[i + 3] / 255.0f;
                    const f32 ab = below.px[i + 3] / 255.0f;
                    if (as <= 0.0f && ab <= 0.0f) continue;
                    Colour cs{ 0.0f, 0.0f, 0.0f, as }, cb{ 0.0f, 0.0f, 0.0f, ab };
                    // Straight colours to blend with, premultiplied ones to composite with.
                    const auto straight = [](u8 value, f32 alpha) {
                        return alpha > 0.0f ? std::clamp(value / 255.0f / alpha, 0.0f, 1.0f) : 0.0f;
                    };
                    cs.r = straight(above.px[i], as);
                    cs.g = straight(above.px[i + 1], as);
                    cs.b = straight(above.px[i + 2], as);
                    cb.r = straight(below.px[i], ab);
                    cb.g = straight(below.px[i + 1], ab);
                    cb.b = straight(below.px[i + 2], ab);

                    Colour blended = cs;
                    if (mode >= kScreen && mode <= kMultiply) {
                        blended = { BlendChannel(mode, cs.r, cb.r),
                                    BlendChannel(mode, cs.g, cb.g),
                                    BlendChannel(mode, cs.b, cb.b), as };
                    } else if (mode >= kHslHue) {
                        blended = BlendNonSeparable(mode, cs, cb);
                        blended.a = as;
                    }
                    // A blend applies only where both are present; elsewhere the source shows
                    // through unblended, which is what the compositing spec means by weighting the
                    // blended colour by the backdrop's alpha.
                    if (mode >= kScreen) {
                        blended.r = cs.r * (1.0f - ab) + blended.r * ab;
                        blended.g = cs.g * (1.0f - ab) + blended.g * ab;
                        blended.b = cs.b * (1.0f - ab) + blended.b * ab;
                    }

                    f32 fs = 1.0f, fb = 1.0f - as;
                    PorterDuff(mode >= kScreen ? u8{ kSrcOver } : mode, as, ab, fs, fb);
                    const f32 alpha = std::clamp(as * fs + ab * fb, 0.0f, 1.0f);
                    const auto channel = [&](f32 s, f32 b) {
                        return static_cast<u8>(std::clamp(
                            (s * as * fs + b * ab * fb) * 255.0f + 0.5f, 0.0f, 255.0f));
                    };
                    target.px[i]     = channel(blended.r, cb.r);
                    target.px[i + 1] = channel(blended.g, cb.g);
                    target.px[i + 2] = channel(blended.b, cb.b);
                    target.px[i + 3] = static_cast<u8>(std::clamp(alpha * 255.0f + 0.5f,
                                                                  0.0f, 255.0f));
                }
                return;
            }

            default: break;
        }

        // A transform node: compose and carry on. Anything unrecognised falls here too and draws
        // nothing, which is the right answer for a paint format written after this reader.
        Affine child = state.transform;
        const u32 sub = Child(paint, format, child);
        if (!sub) return;
        State inner;
        inner.transform = child;
        inner.clip = state.clip;
        Draw(sub, target, inner, depth + 1);
    }

    // ------------------------------------------------------------------------------- the API

    bool ColrGraph::ForegroundOnly(u32 glyph) const {
        const Impl::Base* base = m_Impl->Find(glyph);
        if (!base) return false;
        const Outline none = [](u32, vector::Path&) { return false; };
        ColrGraphWalker walker{ *m_Impl, none };
        walker.Palettes(base->paint, 0);
        return !walker.sawColour;
    }

    bool ColrGraph::Bounds(u32 glyph, f32 scale, const Outline& outline, Rect& out) const {
        const Impl::Base* base = m_Impl->Find(glyph);
        if (!base) return false;

        // The font's own clip box when it has one. It is there precisely so a reader does not have
        // to walk the graph to find out how big the glyph is, and it is authoritative: a glyph is
        // allowed to draw less than its box, and never more.
        Rect box{ { 0.0f, 0.0f }, { 0.0f, 0.0f } };
        if (const Impl::ClipBox* clip = m_Impl->Clip(glyph)) {
            box = clip->box;
        } else {
            ColrGraphWalker walker{ *m_Impl, outline };
            walker.Measure(base->paint, Affine{}, box, 0);
        }
        if (box.size.x <= 0.0f || box.size.y <= 0.0f) return false;

        // Font units, y-up, to pixels, y-down — and outwards to whole pixels, because a box that
        // rounds inwards clips the edge of what it was measuring.
        const f32 x0 = std::floor(box.pos.x * scale);
        const f32 x1 = std::ceil((box.pos.x + box.size.x) * scale);
        const f32 y0 = std::floor(-(box.pos.y + box.size.y) * scale);
        const f32 y1 = std::ceil(-box.pos.y * scale);
        out = { { x0, y0 }, { x1 - x0, y1 - y0 } };
        return out.size.x >= 1.0f && out.size.y >= 1.0f;
    }

    ColrGraph::Picture ColrGraph::Render(u32 glyph, f32 scale, const Outline& outline,
                                         const std::vector<Rgba>& palette) const {
        Picture picture;
        const Impl::Base* base = m_Impl->Find(glyph);
        if (!base) return picture;

        Rect box;
        if (!Bounds(glyph, scale, outline, box)) return picture;
        // A glyph whose box is absurd is a glyph this will not allocate for: the numbers come out
        // of a file, and a 32767-unit box at 128px is 40 000 pixels on a side.
        constexpr f32 kMaxSide = 4096.0f;
        if (box.size.x > kMaxSide || box.size.y > kMaxSide) return picture;

        Canvas canvas;
        canvas.Reset(static_cast<u32>(box.size.x), static_cast<u32>(box.size.y));

        ColrGraphWalker walker{ *m_Impl, outline, &palette, &canvas };
        State state;
        // Font units to pixels, y flipped, with the picture's own top-left as the origin.
        state.transform = Affine{ scale, 0.0f, 0.0f, -scale, -box.pos.x, -box.pos.y };
        walker.Draw(base->paint, canvas, state, 0);

        picture.width  = canvas.width;
        picture.height = canvas.height;
        picture.left   = static_cast<i32>(box.pos.x);
        picture.top    = static_cast<i32>(box.pos.y);
        picture.pixels = std::move(canvas.px);

        // Back to straight alpha, which is what the atlas holds and what every other rasterizer
        // here produces.
        for (std::size_t i = 0; i + 3 < picture.pixels.size(); i += 4) {
            const u32 alpha = picture.pixels[i + 3];
            if (alpha == 0 || alpha == 255) continue;
            for (int c = 0; c < 3; ++c)
                picture.pixels[i + c] = static_cast<u8>(
                    std::min(255u, (picture.pixels[i + c] * 255u + alpha / 2) / alpha));
        }
        return picture;
    }

}
