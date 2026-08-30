#include "Test.h"

#include "vae/base/FileSystem.h"

#include "vae/vector/Svg.h"

#include <cmath>
#include <string>

using namespace vae;
using namespace vae::vector;

namespace {

    // How much of the mask is covered, 0..1. A shape's area is the one number that catches a
    // rasterizer filling the outside, dropping a contour, or getting a winding rule backwards —
    // and it does not care which pixel the antialiasing rounded which way.
    f32 Ink(const Mask& mask) {
        if (mask.Empty()) return 0.0f;
        f64 total = 0.0;
        for (const u8 value : mask.coverage) total += value;
        return static_cast<f32>(total / (255.0 * static_cast<f64>(mask.coverage.size())));
    }

    f32 Alpha(const Bitmap& bitmap, u32 x, u32 y) {
        if (x >= bitmap.width || y >= bitmap.height) return 0.0f;
        return static_cast<f32>(bitmap.pixels[(static_cast<std::size_t>(y) * bitmap.width + x) * 4 + 3])
             / 255.0f;
    }

    Color Pixel(const Bitmap& bitmap, u32 x, u32 y) {
        const std::size_t at = (static_cast<std::size_t>(y) * bitmap.width + x) * 4;
        return { bitmap.pixels[at] / 255.0f, bitmap.pixels[at + 1] / 255.0f,
                 bitmap.pixels[at + 2] / 255.0f, bitmap.pixels[at + 3] / 255.0f };
    }

}

// ------------------------------------------------------------------------------------ geometry

TEST(vector, the_window_icon_that_ships_actually_draws) {
    // The icon is rasterized from SVG at window creation, and every failure path there is a silent
    // early return — a window with no icon looks like a window with no icon, not like a bug. This
    // is what notices that the file stopped parsing.
    const auto source = FileSystem::ReadText(FileSystem::Asset("VAE/assets/icon.svg"));
    CHECK(source.has_value());
    if (!source) return;

    vector::Picture picture;
    std::string error;
    CHECK_MESSAGE(vector::ParseSvg(*source, picture, &error), error);
    CHECK(!picture.Empty());

    const vector::Bitmap bitmap = vector::Render(picture, 64, 64);
    CHECK_EQ(bitmap.width, 64u);
    CHECK_EQ(bitmap.height, 64u);

    std::size_t opaque = 0;
    for (std::size_t i = 3; i < bitmap.pixels.size(); i += 4)
        if (bitmap.pixels[i] > 128) ++opaque;
    // Most of a 64x64 tile, so an icon that renders to nothing — or to a single stray pixel — is
    // a failure rather than a pass.
    CHECK(opaque > 2000);
}

TEST(vector, a_transform_composes_the_way_nesting_reads) {
    // A child scaled by two inside a parent moved by ten is at the parent's offset plus twice its
    // own coordinate — not twice the sum, which is what composing them the other way round gives.
    const Affine inner = Affine::Scaling({ 2.0f, 2.0f });
    const Affine outer = Affine::Translate({ 10.0f, 0.0f });
    const Vec2 p = inner.Then(outer).Apply({ 3.0f, 4.0f });
    CHECK_NEAR(p.x, 16.0f);
    CHECK_NEAR(p.y, 8.0f);

    // A rotation about a point leaves that point alone, which is the whole reason to specify one.
    const Affine spin = Affine::Translate({ -5.0f, -5.0f })
                            .Then(Affine::Rotate(90.0f))
                            .Then(Affine::Translate({ 5.0f, 5.0f }));
    const Vec2 pivot = spin.Apply({ 5.0f, 5.0f });
    CHECK_NEAR_EPS(pivot.x, 5.0f, 1e-4f);
    CHECK_NEAR_EPS(pivot.y, 5.0f, 1e-4f);
}

TEST(vector, a_curve_is_flattened_finer_when_it_will_be_drawn_bigger) {
    Path path;
    path.MoveTo({ 0.0f, 0.0f });
    path.CubicTo({ 0.0f, 50.0f }, { 100.0f, 50.0f }, { 100.0f, 0.0f });

    const std::size_t small = path.Flatten(Affine{}, 0.25f).front().points.size();
    const std::size_t large = path.Flatten(Affine::Scaling({ 8.0f, 8.0f }), 0.25f)
                                  .front().points.size();
    // The same curve, the same tolerance, eight times the size: more segments, because the error
    // is measured where the thing is drawn and not where it was authored.
    CHECK(large > small);
    CHECK(small >= 4);
}

TEST(vector, an_arc_lands_where_it_was_told_to) {
    Path path;
    path.MoveTo({ 0.0f, 50.0f });
    path.ArcTo({ 50.0f, 50.0f }, 0.0f, false, true, { 100.0f, 50.0f });
    const std::vector<Contour> contours = path.Flatten(Affine{}, 0.05f);
    CHECK_EQ(contours.size(), std::size_t(1));

    const Vec2 last = contours.front().points.back();
    CHECK_NEAR_EPS(last.x, 100.0f, 0.05f);
    CHECK_NEAR_EPS(last.y, 50.0f, 0.05f);

    // A semicircle of radius 50 bulges 50 away from its chord, and the sweep flag says which way.
    f32 lowest = 1000.0f;
    for (const Vec2 p : contours.front().points) lowest = std::min(lowest, p.y);
    CHECK_NEAR_EPS(lowest, 0.0f, 0.2f);
}

// ---------------------------------------------------------------------------------- rasterizing

TEST(vector, a_filled_square_covers_exactly_its_own_area) {
    Path path;
    path.MoveTo({ 10.0f, 10.0f });
    path.LineTo({ 90.0f, 10.0f });
    path.LineTo({ 90.0f, 90.0f });
    path.LineTo({ 10.0f, 90.0f });
    path.Close();

    const Mask mask = Fill(path.Flatten(Affine{}), FillRule::NonZero, 100, 100);
    CHECK_EQ(mask.width, 100u);
    // 80x80 of 100x100 is 64% of the bitmap, and the edges are on pixel boundaries so there is no
    // antialiasing to account for.
    CHECK_NEAR_EPS(Ink(mask), 0.64f, 0.005f);
    CHECK_EQ(int(mask.At(50, 50)), 255);
    CHECK_EQ(int(mask.At(5, 5)), 0);
}

TEST(vector, an_edge_between_two_pixels_comes_out_half_covered) {
    Path path;
    path.MoveTo({ 10.5f, 0.0f });
    path.LineTo({ 20.0f, 0.0f });
    path.LineTo({ 20.0f, 20.0f });
    path.LineTo({ 10.5f, 20.0f });
    path.Close();

    const Mask mask = Fill(path.Flatten(Affine{}), FillRule::NonZero, 20, 20);
    // The column the edge runs down is half inside. Hard edges here would mean the rasterizer is
    // sampling pixel centres, which is what makes diagonal lines look like stairs.
    CHECK_NEAR_EPS(static_cast<f32>(mask.At(10, 10)) / 255.0f, 0.5f, 0.02f);
    CHECK_EQ(int(mask.At(11, 10)), 255);
}

TEST(vector, the_fill_rule_decides_whether_a_ring_has_a_hole) {
    // Two squares wound the same way. Nonzero says the inner one is still inside; even-odd says
    // crossing two edges puts you back out. This is the difference between a washer and a disc.
    const auto square = [](f32 inset) {
        Contour c;
        c.points = { { inset, inset }, { 100.0f - inset, inset },
                     { 100.0f - inset, 100.0f - inset }, { inset, 100.0f - inset } };
        return c;
    };
    const std::vector<Contour> rings{ square(0.0f), square(30.0f) };

    const Mask solid = Fill(rings, FillRule::NonZero, 100, 100);
    const Mask holed = Fill(rings, FillRule::EvenOdd, 100, 100);
    CHECK_EQ(int(solid.At(50, 50)), 255);
    CHECK_EQ(int(holed.At(50, 50)), 0);
    CHECK_EQ(int(holed.At(15, 50)), 255);
    CHECK_NEAR_EPS(Ink(holed), 1.0f - 0.16f, 0.01f);
}

TEST(vector, a_stroke_covers_its_width_and_nothing_inside_it) {
    Contour line;
    line.points = { { 10.0f, 50.0f }, { 90.0f, 50.0f } };
    line.closed = false;

    const Mask mask = Fill(Stroke({ line }, 10.0f), FillRule::NonZero, 100, 100);
    CHECK_EQ(int(mask.At(50, 50)), 255);
    CHECK_EQ(int(mask.At(50, 44)), 0);
    CHECK_EQ(int(mask.At(50, 56)), 0);
    // 80 long by 10 wide out of 100x100, with butt caps taking nothing off either end.
    CHECK_NEAR_EPS(Ink(mask), 0.08f, 0.005f);
}

TEST(vector, a_stroked_corner_has_no_hole_in_it) {
    // The join is where a naive stroker fails: two quads that meet at an angle leave a wedge open
    // on the outside, and if the pieces disagree about winding the overlap cancels into a hole.
    Contour bend;
    bend.points = { { 20.0f, 20.0f }, { 50.0f, 50.0f }, { 80.0f, 20.0f } };
    bend.closed = false;

    const Mask mask = Fill(Stroke({ bend }, 12.0f, LineJoin::Round), FillRule::NonZero, 100, 100);
    CHECK_EQ(int(mask.At(50, 50)), 255);
    CHECK_EQ(int(mask.At(50, 54)), 255);      // the outside of the bend, where the wedge would be
    CHECK_EQ(int(mask.At(50, 30)), 0);        // and the inside of the V is still empty
}

// ----------------------------------------------------------------------------------------- svg

TEST(vector, an_svg_of_shapes_reads_as_the_shapes_it_names) {
    const std::string source = R"SVG(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="48" height="48">
  <!-- a comment, which is not a shape -->
  <defs><rect id="ignored" x="0" y="0" width="24" height="24"/></defs>
  <g fill="#ff0000" transform="translate(2 2)">
    <circle cx="10" cy="10" r="8"/>
    <path d="M0 0 L4 0 L4 4 Z" fill="none" stroke="blue" stroke-width="2"/>
  </g>
</svg>)SVG";

    Picture picture;
    std::string error;
    CHECK_MESSAGE(ParseSvg(source, picture, &error), error);
    // The rect inside <defs> defines something for elsewhere to use; drawing it where it sits is
    // the single most visible way to get an SVG wrong.
    CHECK_EQ(picture.shapes.size(), std::size_t(2));
    CHECK_NEAR(picture.size.x, 48.0f);
    CHECK_NEAR(picture.viewBox.size.x, 24.0f);

    const Shape& circle = picture.shapes[0];
    CHECK(circle.hasFill);
    CHECK(!circle.hasStroke);
    CHECK_NEAR(circle.fill.r, 1.0f);
    CHECK_NEAR(circle.fill.g, 0.0f);
    // The group's translate reached it: the circle's own coordinates start at 2, not 0.
    CHECK_NEAR(circle.transform.e, 2.0f);

    const Shape& stroked = picture.shapes[1];
    CHECK(!stroked.hasFill);          // fill="none" on the element beats fill="#f00" on the group
    CHECK(stroked.hasStroke);
    CHECK_NEAR(stroked.stroke.b, 1.0f);
    CHECK_NEAR(stroked.strokeWidth, 2.0f);
}

TEST(vector, path_data_reads_the_shorthand_as_well_as_the_long_way) {
    // Relative commands, an implicit lineto after a moveto, a smooth curve that has to mirror the
    // previous control point, and no separators where the grammar does not need them.
    Picture picture;
    std::string error;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
        <path d="M1 1h8v8h-8z"/>
        <path d="M0,0C1,2 3,2 4,0S7,-2 8,0"/>
    </svg>)SVG", picture, &error));
    CHECK_EQ(picture.shapes.size(), std::size_t(2));

    const Rect box = picture.shapes[0].path.ControlBounds();
    CHECK_NEAR(box.Left(), 1.0f);
    CHECK_NEAR(box.Right(), 9.0f);
    CHECK_NEAR(box.Bottom(), 9.0f);

    const std::vector<Contour> smooth = picture.shapes[1].path.Flatten(Affine{}, 0.01f);
    CHECK_EQ(smooth.size(), std::size_t(1));
    CHECK_NEAR_EPS(smooth.front().points.back().x, 8.0f, 0.01f);
}

TEST(vector, a_rendered_icon_is_the_colour_it_asked_for_and_the_tint_it_was_given) {
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
        <rect x="0" y="0" width="10" height="10" fill="#00ff00"/>
    </svg>)SVG", picture, nullptr));

    const Bitmap own = Render(picture, 32, 32);
    CHECK_EQ(own.width, 32u);
    CHECK_NEAR(Alpha(own, 16, 16), 1.0f);
    const Color middle = Pixel(own, 16, 16);
    CHECK_NEAR_EPS(middle.g, 1.0f, 0.01f);
    CHECK_NEAR_EPS(middle.r, 0.0f, 0.01f);

    // A tint replaces what the author chose, which is what makes an icon obey a theme token
    // instead of the palette whoever drew it happened to have open.
    const Color tint{ 1.0f, 0.0f, 0.0f, 1.0f };
    const Color tinted = Pixel(Render(picture, 32, 32, &tint), 16, 16);
    CHECK_NEAR_EPS(tinted.r, 1.0f, 0.01f);
    CHECK_NEAR_EPS(tinted.g, 0.0f, 0.01f);
}

TEST(vector, a_tint_recolours_what_asked_to_be_told_and_leaves_the_rest) {
    // A two-tone icon: one part says currentColor, the other names a colour outright. A tint that
    // flattened both would turn every piece of artwork into a silhouette.
    Picture two;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 20 10">
        <rect x="0" y="0" width="10" height="10" fill="currentColor"/>
        <rect x="10" y="0" width="10" height="10" fill="#00ff00"/>
    </svg>)SVG", two, nullptr));

    const Color tint{ 1.0f, 0.0f, 0.0f, 1.0f };
    const Bitmap drawn = Render(two, 40, 20, &tint);
    CHECK_NEAR_EPS(Pixel(drawn, 10, 10).r, 1.0f, 0.02f);      // told, and now red
    CHECK_NEAR_EPS(Pixel(drawn, 30, 10).g, 1.0f, 0.02f);      // chose, and still green
    CHECK_NEAR_EPS(Pixel(drawn, 30, 10).r, 0.0f, 0.02f);

    // An icon that never says currentColor has named no parts, so the tint means all of it —
    // otherwise a set that hardcoded black could never follow a theme.
    Picture flat;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
        <rect x="0" y="0" width="10" height="10" fill="#000000"/>
    </svg>)SVG", flat, nullptr));
    CHECK_NEAR_EPS(Pixel(Render(flat, 20, 20, &tint), 10, 10).r, 1.0f, 0.02f);
}

TEST(vector, an_icon_is_fitted_into_its_box_rather_than_stretched) {
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
        <circle cx="5" cy="5" r="5" fill="black"/>
    </svg>)SVG", picture, nullptr));

    // A square icon in a wide box keeps its shape and gets bars, rather than becoming an ellipse.
    const Bitmap wide = Render(picture, 80, 40);
    CHECK_NEAR_EPS(Alpha(wide, 40, 20), 1.0f, 0.01f);
    CHECK_NEAR_EPS(Alpha(wide, 4, 20), 0.0f, 0.01f);     // the bar on the left is empty
    CHECK(Alpha(wide, 40, 2) > 0.5f);                    // the circle still reaches the top edge
    CHECK_NEAR_EPS(Alpha(wide, 22, 2), 0.0f, 0.05f);     // and the corners of its box are not it
    // Stretched, that circle would be 80 wide: the point three quarters across would be inside it.
    CHECK_NEAR_EPS(Alpha(wide, 68, 20), 0.0f, 0.01f);
}

TEST(vector, something_that_is_not_an_svg_says_so_instead_of_drawing_nothing) {
    Picture picture;
    std::string error;
    CHECK(!ParseSvg("<html><body>not a picture</body></html>", picture, &error));
    CHECK(!error.empty());

    // An SVG whose only content is a gradient nobody can read is still a failure with a reason,
    // not an empty rectangle the designer has to guess about.
    CHECK(!ParseSvg(R"SVG(<svg viewBox="0 0 4 4"><text x="0" y="0">hello</text></svg>)SVG",
                    picture, &error));
    CHECK(error.find("text") != std::string::npos);
}

TEST(vector, a_real_icon_comes_out_looking_like_one) {
    // Lucide's "activity" plus a ring: currentColor, a round-capped polyline and a circle, which
    // between them exercise the stroker's caps, joins and closed-contour path.
    const std::string source = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"
        width="24" height="24" fill="none" stroke="currentColor" stroke-width="2"
        stroke-linecap="round" stroke-linejoin="round">
      <path d="M3 12h4l3 8 4-16 3 8h4"/>
      <circle cx="12" cy="12" r="10.5" stroke-width="1"/>
    </svg>)SVG";

    Picture picture;
    std::string error;
    CHECK_MESSAGE(ParseSvg(source, picture, &error), error);
    CHECK_EQ(picture.shapes.size(), std::size_t(2));
    CHECK(picture.shapes[0].strokeFollowsText);
    CHECK_NEAR(picture.shapes[1].strokeWidth, 1.0f);     // the element's width beats the root's

    const Color ink{ 1.0f, 1.0f, 1.0f, 1.0f };
    const Bitmap drawn = Render(picture, 96, 96, &ink);
    CHECK_EQ(drawn.width, 96u);

    // Ink where the polyline runs, nothing in the corners outside the ring.
    CHECK(Alpha(drawn, 20, 48) > 0.5f);
    CHECK_NEAR_EPS(Alpha(drawn, 3, 3), 0.0f, 0.02f);
    // And nothing inside the ring away from the line: a circle with `fill="none"` that comes out
    // as a disc is the mistake that makes every outline icon a blob.
    CHECK_NEAR_EPS(Alpha(drawn, 70, 74), 0.0f, 0.02f);

    f64 total = 0.0;
    for (std::size_t i = 3; i < drawn.pixels.size(); i += 4) total += drawn.pixels[i];
    const f32 covered = static_cast<f32>(total / (255.0 * (drawn.pixels.size() / 4)));
    CHECK(covered > 0.03f);
    CHECK(covered < 0.40f);
}

// --- gradients ----------------------------------------------------------------------------------
//
// Skipped until now, and honestly so: a wrong picture is worse than a missing one. What made them
// worth reading is that a colour font's glyphs are SVG documents, and almost every one of them is
// mostly gradient.

TEST(vector, a_linear_gradient_is_read_and_drawn_across_the_shape) {
    Picture picture;
    std::string error;
    CHECK_MESSAGE(ParseSvg(R"SVG(<svg viewBox="0 0 20 20">
      <defs>
        <linearGradient id="sky" x1="0" y1="0" x2="1" y2="0">
          <stop offset="0" stop-color="#ff0000"/>
          <stop offset="1" stop-color="#0000ff"/>
        </linearGradient>
      </defs>
      <rect x="0" y="0" width="20" height="20" fill="url(#sky)"/>
    </svg>)SVG", picture, &error), error);

    CHECK_EQ(picture.gradients.size(), std::size_t(1));
    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_EQ(picture.shapes[0].fillGradient, 0);
    CHECK_EQ(picture.gradients[0].stops.size(), std::size_t(2));
    CHECK(!picture.gradients[0].userSpace);       // objectBoundingBox is the default

    const Bitmap bitmap = Render(picture, 40, 40);
    CHECK_EQ(bitmap.width, 40u);
    const auto at = [&](u32 x, u32 y, int channel) {
        return bitmap.pixels[(static_cast<std::size_t>(y) * bitmap.width + x) * 4 + channel];
    };
    // Red on the left, blue on the right, and a real mix in the middle — which is the difference
    // between a gradient and a shape that took the first stop's colour.
    CHECK(at(2, 20, 0) > 200);
    CHECK(at(2, 20, 2) < 60);
    CHECK(at(37, 20, 2) > 200);
    CHECK(at(37, 20, 0) < 60);
    CHECK(at(20, 20, 0) > 60);
    CHECK(at(20, 20, 0) < 200);
}

TEST(vector, a_radial_gradient_is_a_circle_around_its_centre) {
    Picture picture;
    std::string error;
    CHECK_MESSAGE(ParseSvg(R"SVG(<svg viewBox="0 0 20 20">
      <radialGradient id="ball" cx="0.5" cy="0.5" r="0.5">
        <stop offset="0" stop-color="white"/>
        <stop offset="1" stop-color="black"/>
      </radialGradient>
      <rect x="0" y="0" width="20" height="20" fill="url(#ball)"/>
    </svg>)SVG", picture, &error), error);
    CHECK_EQ(picture.gradients.size(), std::size_t(1));
    CHECK(picture.gradients[0].kind == Gradient::Kind::Radial);

    const Bitmap bitmap = Render(picture, 40, 40);
    const auto red = [&](u32 x, u32 y) {
        return bitmap.pixels[(static_cast<std::size_t>(y) * bitmap.width + x) * 4];
    };
    // Bright in the middle and dark at every edge — a linear gradient read as a radial one would
    // be bright down one whole side.
    CHECK(red(20, 20) > 200);
    CHECK(red(1, 20) < 80);
    CHECK(red(38, 20) < 80);
    CHECK(red(20, 1) < 80);
    CHECK(red(20, 38) < 80);
}

TEST(vector, a_gradient_may_be_defined_after_the_shape_that_uses_it) {
    // Valid SVG, and the reason the names are resolved once the whole file has been read rather
    // than at the moment the shape is built.
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <rect width="10" height="10" fill="url(#later)"/>
      <defs><linearGradient id="later"><stop offset="0" stop-color="red"/>
                                        <stop offset="1" stop-color="lime"/></linearGradient></defs>
    </svg>)SVG", picture));
    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_EQ(picture.shapes[0].fillGradient, 0);
}

TEST(vector, a_gradient_inherits_the_stops_of_the_one_it_references) {
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <defs>
        <linearGradient id="base"><stop offset="0" stop-color="red"/>
                                  <stop offset="1" stop-color="blue"/></linearGradient>
        <linearGradient id="tilted" xlink:href="#base" x1="0" y1="0" x2="0" y2="1"/>
      </defs>
      <rect width="10" height="10" fill="url(#tilted)"/>
    </svg>)SVG", picture));
    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK(picture.shapes[0].fillGradient >= 0);
    if (picture.shapes[0].fillGradient < 0) return;
    const Gradient& used = picture.gradients[static_cast<std::size_t>(picture.shapes[0].fillGradient)];
    CHECK_EQ(used.stops.size(), std::size_t(2));
    CHECK_NEAR(used.to.y, 1.0f);
}

TEST(vector, a_url_that_names_something_unreadable_still_takes_the_tint) {
    // A pattern, a filter, a paint server this does not read. The shape has to end up somewhere,
    // and "whatever colour the theme says" is the same answer it got before gradients existed.
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <rect width="10" height="10" fill="url(#nothing)"/>
    </svg>)SVG", picture));
    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_EQ(picture.shapes[0].fillGradient, -1);
    CHECK(picture.shapes[0].fillFollowsText);
    CHECK(picture.FollowsText());
}

TEST(vector, only_the_named_subtree_is_kept_when_one_is_asked_for) {
    // How a colour font asks for one glyph out of a document that draws several.
    const std::string source = R"SVG(<svg viewBox="0 0 10 10">
      <defs><linearGradient id="g"><stop offset="0" stop-color="red"/>
                                   <stop offset="1" stop-color="blue"/></linearGradient></defs>
      <g id="glyph1"><rect width="4" height="4" fill="url(#g)"/></g>
      <g id="glyph2"><circle cx="8" cy="8" r="2" fill="green"/></g>
    </svg>)SVG";

    Picture both;
    CHECK(ParseSvg(source, both));
    CHECK_EQ(both.shapes.size(), std::size_t(2));

    Picture one;
    CHECK(ParseSvg(source, one, nullptr, "glyph2"));
    CHECK_EQ(one.shapes.size(), std::size_t(1));
    CHECK_NEAR(one.shapes[0].fill.g, 0.5019608f);

    // And the definitions outside it are still read: the gradient lives in <defs>, which is in no
    // glyph's subtree at all.
    Picture gradient;
    CHECK(ParseSvg(source, gradient, nullptr, "glyph1"));
    CHECK_EQ(gradient.shapes.size(), std::size_t(1));
    CHECK_EQ(gradient.shapes[0].fillGradient, 0);

    // An id that is not there means the document is one glyph and did not say so.
    Picture missing;
    CHECK(ParseSvg(source, missing, nullptr, "glyph9"));
    CHECK_EQ(missing.shapes.size(), std::size_t(2));
}

// --- <style> ------------------------------------------------------------------------------------
//
// The one part of CSS an SVG reader cannot skip. Illustrator, Inkscape's "optimised" output and
// every OpenType-SVG glyph document built from them put the fills in a `<style>` block and name
// them from the shapes — so a reader that ignores it draws the whole picture in the default black.

TEST(vector, a_style_block_paints_the_shapes_that_name_its_class) {
    Picture picture;
    std::string error;
    CHECK_MESSAGE(ParseSvg(R"SVG(<svg viewBox="0 0 20 20">
      <style type="text/css"><![CDATA[
        /* what an exporter writes */
        .st0 { fill: #ff0000; }
        .st1 { fill: #0000ff; fill-rule: evenodd; }
      ]]></style>
      <rect class="st0" x="0" y="0" width="10" height="20"/>
      <rect class="st1" x="10" y="0" width="10" height="20"/>
    </svg>)SVG", picture, &error), error);

    CHECK_EQ(picture.shapes.size(), std::size_t(2));
    CHECK_NEAR(picture.shapes[0].fill.r, 1.0f);
    CHECK_NEAR(picture.shapes[0].fill.b, 0.0f);
    CHECK_NEAR(picture.shapes[1].fill.b, 1.0f);
    CHECK_NEAR(picture.shapes[1].fill.r, 0.0f);
    // Not only paint: a rule says anything a presentation attribute could have.
    CHECK(picture.shapes[1].rule == FillRule::EvenOdd);
    CHECK(picture.shapes[0].rule == FillRule::NonZero);
}

TEST(vector, a_shape_in_two_classes_takes_both_of_them) {
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <style>.fill{fill:#00ff00}.edge{stroke:#ff0000;stroke-width:3}</style>
      <rect class="fill edge" x="1" y="1" width="8" height="8"/>
    </svg>)SVG", picture));

    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_NEAR(picture.shapes[0].fill.g, 1.0f);
    CHECK(picture.shapes[0].hasStroke);
    CHECK_NEAR(picture.shapes[0].strokeWidth, 3.0f);
}

TEST(vector, specificity_decides_when_two_rules_disagree) {
    // A tag rule loses to a class rule, a class rule loses to an id rule, and the element's own
    // `style` beats all of them — while a plain `fill=` attribute loses to every rule. Getting
    // this backwards is how a themed icon set comes out in the wrong colour and still looks fine.
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 40 10">
      <style>
        rect { fill: #ff0000 }
        .mid { fill: #00ff00 }
        #top { fill: #0000ff }
      </style>
      <rect x="0" y="0" width="10" height="10"/>
      <rect class="mid" x="10" y="0" width="10" height="10" fill="#ffffff"/>
      <rect id="top" class="mid" x="20" y="0" width="10" height="10"/>
      <rect id="top" class="mid" x="30" y="0" width="10" height="10" style="fill:#000000"/>
    </svg>)SVG", picture));

    CHECK_EQ(picture.shapes.size(), std::size_t(4));
    CHECK_NEAR(picture.shapes[0].fill.r, 1.0f);      // the tag rule
    CHECK_NEAR(picture.shapes[1].fill.g, 1.0f);      // the class rule, over the attribute
    CHECK_NEAR(picture.shapes[1].fill.r, 0.0f);
    CHECK_NEAR(picture.shapes[2].fill.b, 1.0f);      // the id rule, over the class
    CHECK_NEAR(picture.shapes[3].fill.r, 0.0f);      // inline style, over everything
    CHECK_NEAR(picture.shapes[3].fill.g, 0.0f);
    CHECK_NEAR(picture.shapes[3].fill.b, 0.0f);
}

TEST(vector, a_stylesheet_below_the_shapes_still_paints_them) {
    // A stylesheet applies to the whole document wherever it sits in it, and a one-pass reader
    // that applied rules as it met them would paint everything above the block black.
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <rect class="late" x="1" y="1" width="8" height="8"/>
      <style>.late{fill:#00ffff}</style>
    </svg>)SVG", picture));

    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_NEAR(picture.shapes[0].fill.g, 1.0f);
    CHECK_NEAR(picture.shapes[0].fill.b, 1.0f);
}

TEST(vector, a_class_rule_reaches_a_shape_through_the_group_it_is_on) {
    // Because the rule is applied to the element's attributes, inheritance down the tree is the
    // one that was already there rather than a second mechanism.
    Picture picture;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <style>.group{fill:#ff00ff}</style>
      <g class="group"><rect x="1" y="1" width="8" height="8"/></g>
    </svg>)SVG", picture));

    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_NEAR(picture.shapes[0].fill.r, 1.0f);
    CHECK_NEAR(picture.shapes[0].fill.b, 1.0f);
    CHECK_NEAR(picture.shapes[0].fill.g, 0.0f);
}

TEST(vector, a_selector_this_reader_cannot_match_leaves_the_shape_alone) {
    // Half-applying a descendant selector paints things that were never selected. Saying so and
    // drawing the default is the answer; `error` is where it says so.
    Picture picture;
    std::string error;
    CHECK(ParseSvg(R"SVG(<svg viewBox="0 0 10 10">
      <style>
        @media print { .any { fill: #ff0000 } }
        g .deep { fill: #ff0000 }
        .plain { stroke: #00ff00; stroke-width: 2 }
      </style>
      <rect class="deep plain" x="1" y="1" width="8" height="8"/>
    </svg>)SVG", picture, &error));

    CHECK_EQ(picture.shapes.size(), std::size_t(1));
    CHECK_NEAR(picture.shapes[0].fill.r, 0.0f);      // black, the default, not the rule's red
    CHECK(picture.shapes[0].hasStroke);              // and the simple rule beside it still applied
    CHECK(!error.empty());
}
