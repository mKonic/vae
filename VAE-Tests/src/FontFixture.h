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

    // The bytes of the bundled outline font every fixture is built on, or empty if it is missing.
    const std::vector<u8>& BaseFont();

}
