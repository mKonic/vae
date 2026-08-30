#pragma once

#include "vae/base/Base.h"

#include <string>
#include <vector>

// Colour fonts that do not exist on this machine, built here so the readers can be tested anyway.
//
// Noto Color Emoji ships CBDT/CBLC and is on every Linux desktop, so that reader has a real font to
// be checked against. `sbix` is Apple's and `COLR`/`CPAL` is mostly Windows and web fonts; neither
// is installed here, and vendoring a 10 MB emoji font for a test is not a trade worth making.
//
// So the fixtures are made instead: a real outline font — the bundled JetBrains Mono — with a real
// colour table injected into its table directory. Nothing is faked below the table being tested.
// The font that comes out is a font: stb_truetype reads its outlines and HarfBuzz shapes it.
namespace vae::fixture {

    // JetBrains Mono with an `sbix` table: one strike at `ppem`, one PNG, on the glyph for `letter`.
    // The picture is a solid opaque square of the given colour, `size` pixels on a side, placed
    // with its bottom-left `originX`/`originY` pixels from the pen. When `duplicate` is given, that
    // character gets a `dupe` entry pointing at `letter` — the format's way of shipping one image
    // for several characters — and its glyph must sort after `letter`'s.
    struct SbixSpec {
        char letter = 'A';
        char duplicate = 0;
        u32  ppem = 128;
        u32  size = 64;
        u8   red = 220, green = 40, blue = 40;
        i32  originX = 0, originY = 0;
        // Fully transparent pixels around the square, whose colour every PNG encoder writes as
        // black. A downscale that averages them in without weighting by alpha puts a dark halo
        // round the picture, and only a border makes that visible.
        u32  border = 0;
    };

    std::vector<u8> SbixFont(const SbixSpec& spec);

    // JetBrains Mono with `COLR`/`CPAL`: the glyph for `base` is drawn as the glyphs for the
    // characters in `layers`, back to front, each in the matching colour from `palette`. A palette
    // index of 0xFFFF names the text colour instead, which is the case the atlas cannot hold.
    struct ColrLayerSpec { char glyph; u32 palette; };
    struct PaletteEntry  { u8 r, g, b, a; };

    std::vector<u8> ColrFont(char base, const std::vector<ColrLayerSpec>& layers,
                             const std::vector<PaletteEntry>& palette);

    // JetBrains Mono with a COLR **version 1** table: the glyph for `base` is drawn by a paint
    // graph rather than by a layer list. One builder covers every case worth checking, because the
    // interesting part is always the same — which paints, nested how — and a separate fixture per
    // shape would be six copies of the same offset arithmetic.
    //
    // The graph built is, from the outside in:
    //
    //     [PaintComposite(mode) with `under` beneath]      when `composite` is given
    //       [PaintColrLayers over the layer list]          when `viaLayers`
    //         [a transform]                                when `transform` is not None
    //           PaintGlyph(shape) -> the fill
    struct ColrV1Spec {
        char base = 'A';
        char shape = 'A';               // whose outline the fill is clipped to
        char under = 0;                 // a second glyph, drawn beneath in `underPalette`

        enum class Fill : u8 { Solid, Linear, Radial, Sweep };
        Fill fill = Fill::Solid;
        u32 palette = 0;                // Solid: the colour; gradients: the first stop
        u32 palette2 = 1;               // gradients: the last stop
        u8  extend = 0;                 // 0 pad, 1 repeat, 2 reflect
        u32 underPalette = 1;
        f32 alpha = 1.0f;

        enum class Transform : u8 { None, Translate, Scale, Rotate, Skew, Matrix };
        Transform transform = Transform::None;
        f32 amount = 0.0f;              // units for Translate, a factor for Scale, degrees for the
                                        // rest; Matrix uses it as a uniform scale

        bool viaLayers = false;         // reach the paint through PaintColrLayers + the layer list
        i32 composite = -1;             // a CompositeMode, or -1 for no PaintComposite at all

        // A clip box in font units. Left at zero, the reader has to measure the graph itself.
        i32 clipXMin = 0, clipYMin = 0, clipXMax = 0, clipYMax = 0;
    };

    std::vector<u8> ColrV1Font(const ColrV1Spec& spec, const std::vector<PaletteEntry>& palette);

    // JetBrains Mono with an OpenType-`SVG ` table: an SVG document holding a filled rectangle
    // for `letter`, and a second one for `second` when it is given. The interesting axes are how
    // the document is stored and how the glyph is found inside it, so those are what this varies.
    //
    // The rectangle is in font units with the y axis pointing **down** and the origin at the pen,
    // which is the coordinate system the format defines and the one thing a reader is most likely
    // to get upside down.
    struct SvgSpec {
        char letter = 'A';
        char second = 0;                // a second glyph, drawn in `secondColour`
        bool gzip = false;              // the document is gzipped, as most real ones are
        bool separateRecords = false;   // `second` gets its own document instead of sharing one
        bool currentColour = false;     // the whole drawing asks for the colour of the text
        bool anonymous = false;         // no ids at all: one document, one glyph, nothing to find

        PaletteEntry colour{ 220, 40, 40, 255 };
        PaletteEntry secondColour{ 40, 80, 220, 255 };
        // The rectangle, in font units, y-down from the pen.
        i32 x = 100, y = -700, width = 600, height = 600;
        // Written into the table instead of a document, to check that a reader refuses a document
        // it cannot parse rather than drawing something wrong.
        std::string instead;
    };

    std::vector<u8> SvgFont(const SvgSpec& spec);

    // The bytes of the bundled outline font every fixture is built on, or empty if it is missing.
    const std::vector<u8>& BaseFont();

}
