#include "Test.h"

#include "vae/draw/DrawList.h"

using namespace vae;
using namespace vae::draw;

namespace {
    Rect QuadRect(const DrawList& list, std::size_t i) {
        const Vec4& r = list.Quads()[i].rect;
        return Rect{ { r.x, r.y }, { r.z, r.w } };
    }
    Rect ClipRect(const DrawList& list, std::size_t i) {
        const Vec4& r = list.Quads()[i].clipRect;
        return Rect{ { r.x, r.y }, { r.z, r.w } };
    }
}

TEST(draw, consecutive_same_kind_primitives_share_one_batch) {
    DrawList list;
    list.Reset();
    for (int i = 0; i < 5; ++i)
        list.AddRect(Rect{ { f32(i) * 10.0f, 0.0f }, { 8.0f, 8.0f } }, Paint::Solid({1,1,1,1}));

    CHECK_EQ(list.Quads().size(), 5u);
    CHECK_EQ(list.Batches().size(), 1u);
    CHECK_EQ(list.Batches()[0].count, 5u);
}

TEST(draw, changing_primitive_kind_cuts_a_batch) {
    // Painter order is submission order, so a shadow between two rects must NOT be hoisted into
    // one shadow batch — that would draw it on top of the first rect.
    DrawList list;
    list.Reset();
    list.AddRect(Rect{ {0,0}, {10,10} }, Paint::Solid({1,1,1,1}));
    list.AddShadow(Rect{ {0,0}, {10,10} }, ShadowSpec{});
    list.AddRect(Rect{ {0,0}, {10,10} }, Paint::Solid({1,1,1,1}));

    CHECK_EQ(list.Batches().size(), 3u);
    CHECK(list.Batches()[0].kind == PrimitiveKind::Quad);
    CHECK(list.Batches()[1].kind == PrimitiveKind::Shadow);
    CHECK(list.Batches()[2].kind == PrimitiveKind::Quad);
    // The second quad batch indexes into the quad array after the first.
    CHECK_EQ(list.Batches()[2].first, 1u);
}

TEST(draw, empty_rects_are_dropped) {
    DrawList list;
    list.Reset();
    list.AddRect(Rect{ {0,0}, {0,10} }, Paint::Solid({1,1,1,1}));
    list.AddRect(Rect{ {0,0}, {10,-4} }, Paint::Solid({1,1,1,1}));
    CHECK_EQ(list.Quads().size(), 0u);
}

TEST(draw, nested_clips_intersect) {
    DrawList list;
    list.Reset();
    list.PushClip(Rect{ { 100.0f, 100.0f }, { 200.0f, 200.0f } });
    list.PushClip(Rect{ { 150.0f, 50.0f },  { 200.0f, 100.0f } });
    list.AddRect(Rect{ {0,0}, {1000,1000} }, Paint::Solid({1,1,1,1}));
    list.PopClip();
    list.PopClip();

    const Rect clip = ClipRect(list, 0);
    CHECK_NEAR(clip.Left(),   150.0f);
    CHECK_NEAR(clip.Top(),    100.0f);
    CHECK_NEAR(clip.Right(),  300.0f);
    CHECK_NEAR(clip.Bottom(), 150.0f);
}

TEST(draw, innermost_rounded_clip_wins) {
    DrawList list;
    list.Reset();
    list.PushClip(Rect{ {0,0}, {100,100} }, Corners{ 4.0f });
    list.PushClip(Rect{ {0,0}, {100,100} }, Corners{ 20.0f });
    list.AddRect(Rect{ {0,0}, {50,50} }, Paint::Solid({1,1,1,1}));
    list.PopClip();
    list.PopClip();
    CHECK_NEAR(list.Quads()[0].clipRadii.x, 20.0f);
}

TEST(draw, unclipped_primitives_get_a_clip_that_cannot_cut_anything) {
    DrawList list;
    list.Reset();
    list.AddRect(Rect{ { 10.0f, 10.0f }, { 20.0f, 20.0f } }, Paint::Solid({1,1,1,1}));
    const Rect clip = ClipRect(list, 0);
    CHECK(clip.Left()   < -1000.0f);
    CHECK(clip.Right()  >  1000.0f);
    CHECK(clip.Bottom() >  1000.0f);
}

TEST(draw, transform_scales_position_size_radii_and_border) {
    DrawList list;
    list.Reset();
    list.PushTransform({ 2.0f, 2.0f }, { 100.0f, 50.0f });
    list.AddRect(Rect{ { 10.0f, 10.0f }, { 40.0f, 20.0f } }, Paint::Solid({1,1,1,1}),
                 Corners{ 6.0f }, Stroke{ 3.0f, {0,0,0,1} });
    list.PopTransform();

    const Rect r = QuadRect(list, 0);
    CHECK_NEAR(r.pos.x, 120.0f);        // 10*2 + 100
    CHECK_NEAR(r.pos.y, 70.0f);         // 10*2 + 50
    CHECK_NEAR(r.size.x, 80.0f);
    CHECK_NEAR(list.Quads()[0].radii.x, 12.0f);
    CHECK_NEAR(list.Quads()[0].params.x, 6.0f);   // border width scaled too
}

TEST(draw, nested_transforms_compose) {
    DrawList list;
    list.Reset();
    list.PushTransform({ 2.0f, 2.0f }, { 10.0f, 0.0f });
    list.PushTransform({ 0.5f, 0.5f }, { 4.0f, 0.0f });    // net scale 1, offset 10 + 2*4
    list.AddRect(Rect{ { 0.0f, 0.0f }, { 10.0f, 10.0f } }, Paint::Solid({1,1,1,1}));
    list.PopTransform();
    list.PopTransform();

    const Rect r = QuadRect(list, 0);
    CHECK_NEAR(r.pos.x, 18.0f);
    CHECK_NEAR(r.size.x, 10.0f);
}

TEST(draw, clips_are_recorded_in_transformed_space) {
    DrawList list;
    list.Reset();
    list.PushTransform({ 2.0f, 2.0f }, { 0.0f, 0.0f });
    list.PushClip(Rect{ { 10.0f, 10.0f }, { 10.0f, 10.0f } });
    list.AddRect(Rect{ { 0.0f, 0.0f }, { 100.0f, 100.0f } }, Paint::Solid({1,1,1,1}));
    list.PopClip();
    list.PopTransform();

    const Rect clip = ClipRect(list, 0);
    CHECK_NEAR(clip.pos.x, 20.0f);
    CHECK_NEAR(clip.size.x, 20.0f);
}

TEST(draw, shadow_applies_offset_and_spread) {
    DrawList list;
    list.Reset();
    ShadowSpec s;
    s.offset = { 4.0f, 8.0f };
    s.spread = 2.0f;
    s.blur = 10.0f;
    list.AddShadow(Rect{ { 100.0f, 100.0f }, { 50.0f, 50.0f } }, s);

    const Vec4& r = list.Shadows()[0].rect;
    CHECK_NEAR(r.x, 102.0f);        // 100 - spread + offset.x
    CHECK_NEAR(r.y, 106.0f);        // 100 - spread + offset.y
    CHECK_NEAR(r.z, 54.0f);         // grown by spread on both sides
    CHECK_NEAR(list.Shadows()[0].params.x, 5.0f);   // blur radius -> sigma
}

TEST(draw, axis_aligned_lines_become_rects) {
    DrawList list;
    list.Reset();
    list.AddLine({ 10.0f, 50.0f }, { 110.0f, 50.0f }, 2.0f, { 1,1,1,1 });
    CHECK_EQ(list.Quads().size(), 1u);
    const Rect r = QuadRect(list, 0);
    CHECK_NEAR(r.pos.x, 10.0f);
    CHECK_NEAR(r.size.x, 100.0f);
    CHECK_NEAR(r.pos.y, 49.0f);
    CHECK_NEAR(r.size.y, 2.0f);
}

TEST(draw, reset_clears_everything_including_the_stacks) {
    DrawList list;
    list.Reset();
    list.PushClip(Rect{ {0,0}, {10,10} });
    list.PushTransform({ 3.0f, 3.0f }, { 5.0f, 5.0f });
    list.AddRect(Rect{ {0,0}, {10,10} }, Paint::Solid({1,1,1,1}));

    list.Reset();
    CHECK(list.Empty());
    CHECK_EQ(list.Quads().size(), 0u);

    // A leaked transform or clip from the previous frame would show up here.
    list.AddRect(Rect{ { 1.0f, 1.0f }, { 2.0f, 2.0f } }, Paint::Solid({1,1,1,1}));
    const Rect r = QuadRect(list, 0);
    CHECK_NEAR(r.pos.x, 1.0f);
    CHECK_NEAR(r.size.x, 2.0f);
    CHECK(ClipRect(list, 0).Left() < -1000.0f);
}
