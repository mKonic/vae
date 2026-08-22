#include "Test.h"

#include "vae/layout/LayoutTree.h"

using namespace vae;
using namespace vae::layout;

namespace {

    LayoutStyle Stack(Axis axis, f32 gap = 0.0f, Edges padding = {}) {
        LayoutStyle s;
        s.mode = LayoutMode::Stack;
        s.axis = axis;
        s.gap = gap;
        s.padding = padding;
        return s;
    }

    LayoutStyle Grid(u16 columns, f32 gap = 0.0f, f32 rowGap = 0.0f, Edges padding = {}) {
        LayoutStyle s;
        s.mode = LayoutMode::Grid;
        s.columns = columns;
        s.gap = gap;
        s.rowGap = rowGap;
        s.padding = padding;
        return s;
    }

    LayoutStyle Box(Size w, Size h) {
        LayoutStyle s;
        s.width = w;
        s.height = h;
        return s;
    }

}

// ------------------------------------------------------------------ sizing

TEST(layout, fixed_sizes_are_honoured) {
    LayoutTree tree;
    const u32 root = tree.Add(Box(Size::Px(200.0f), Size::Px(100.0f)));
    tree.Compute(root, { 1000.0f, 1000.0f });
    CHECK_NEAR(tree.NodeRect(root).size.x, 200.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 100.0f);
}

TEST(layout, hug_sums_children_along_the_stack_axis) {
    LayoutTree tree;
    const u32 root = tree.Add(Stack(Axis::Row, 10.0f));
    for (int i = 0; i < 3; ++i)
        tree.Add(Box(Size::Px(50.0f), Size::Px(30.0f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    // 3 * 50 + 2 * 10 gap
    CHECK_NEAR(tree.NodeRect(root).size.x, 170.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 30.0f);
}

TEST(layout, padding_expands_a_hugging_parent) {
    LayoutTree tree;
    const u32 root = tree.Add(Stack(Axis::Column, 0.0f, Edges{ 8.0f, 12.0f, 8.0f, 12.0f }));
    tree.Add(Box(Size::Px(100.0f), Size::Px(40.0f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(root).size.x, 116.0f);   // 100 + 8 + 8
    CHECK_NEAR(tree.NodeRect(root).size.y, 64.0f);    // 40 + 12 + 12
    CHECK_NEAR(tree.NodeRect(tree.Children(root)[0]).pos.x, 8.0f);
    CHECK_NEAR(tree.NodeRect(tree.Children(root)[0]).pos.y, 12.0f);
}

TEST(layout, fill_children_split_the_leftover_by_weight) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 10.0f);
    rootStyle.width = Size::Px(310.0f);
    rootStyle.height = Size::Px(50.0f);
    const u32 root = tree.Add(rootStyle);

    tree.Add(Box(Size::Px(100.0f), Size::Fill()), root);   // fixed
    tree.Add(Box(Size::Fill(1.0f), Size::Fill()), root);   // 1 share
    tree.Add(Box(Size::Fill(3.0f), Size::Fill()), root);   // 3 shares
    tree.Compute(root, { 1000.0f, 1000.0f });

    // 310 - 100 fixed - 20 gap = 190 to split 1:3
    const auto& kids = tree.Children(root);
    CHECK_NEAR(tree.NodeRect(kids[1]).size.x, 47.5f);
    CHECK_NEAR(tree.NodeRect(kids[2]).size.x, 142.5f);
    CHECK_NEAR(tree.NodeRect(kids[2]).pos.x, 167.5f);      // 100 + 10 + 47.5 + 10
}

TEST(layout, hug_inside_fill_resolves_both) {
    // The classic case that breaks naive solvers: a Fill column whose child hugs its own content.
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row);
    rootStyle.width = Size::Px(400.0f);
    rootStyle.height = Size::Px(200.0f);
    const u32 root = tree.Add(rootStyle);

    LayoutStyle columnStyle = Stack(Axis::Column, 4.0f);
    columnStyle.width = Size::Fill();
    columnStyle.height = Size::Hug();
    const u32 column = tree.Add(columnStyle, root);

    tree.Add(Box(Size::Px(30.0f), Size::Px(20.0f)), column);
    tree.Add(Box(Size::Px(30.0f), Size::Px(25.0f)), column);
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(column).size.x, 400.0f);   // filled
    CHECK_NEAR(tree.NodeRect(column).size.y, 49.0f);    // hugged: 20 + 4 + 25
}

TEST(layout, percent_resolves_against_the_parent_content_box) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Column, 0.0f, Edges{ 10.0f });
    rootStyle.width = Size::Px(200.0f);
    rootStyle.height = Size::Px(100.0f);
    const u32 root = tree.Add(rootStyle);
    tree.Add(Box(Size::Percent(0.5f), Size::Percent(0.25f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    // Content box is 180x80 after padding.
    const u32 child = tree.Children(root)[0];
    CHECK_NEAR(tree.NodeRect(child).size.x, 90.0f);
    CHECK_NEAR(tree.NodeRect(child).size.y, 20.0f);
}

TEST(layout, percent_chains_through_nested_parents) {
    LayoutTree tree;
    LayoutStyle a = Stack(Axis::Column);
    a.width = Size::Px(400.0f); a.height = Size::Px(400.0f);
    const u32 root = tree.Add(a);

    LayoutStyle b = Stack(Axis::Column);
    b.width = Size::Percent(0.5f); b.height = Size::Percent(0.5f);
    const u32 mid = tree.Add(b, root);

    tree.Add(Box(Size::Percent(0.5f), Size::Percent(0.5f)), mid);
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(mid).size.x, 200.0f);
    CHECK_NEAR(tree.NodeRect(tree.Children(mid)[0]).size.x, 100.0f);
}

TEST(layout, min_and_max_clamp_after_sizing) {
    LayoutTree tree;
    LayoutStyle style = Box(Size::Px(500.0f), Size::Px(10.0f));
    style.maxSize = { 300.0f, kUnbounded };
    style.minSize = { 0.0f, 50.0f };
    const u32 root = tree.Add(style);
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(root).size.x, 300.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 50.0f);
}

TEST(layout, aspect_ratio_derives_the_free_axis) {
    LayoutTree tree;
    LayoutStyle style = Box(Size::Px(300.0f), Size::Hug());
    style.aspectRatio = 16.0f / 9.0f;
    const u32 root = tree.Add(style);
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(root).size.x, 300.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 300.0f * 9.0f / 16.0f);
}

TEST(layout, an_explicit_size_outranks_the_aspect_ratio) {
    LayoutTree tree;
    LayoutStyle style = Box(Size::Px(300.0f), Size::Px(300.0f));
    style.aspectRatio = 16.0f / 9.0f;
    const u32 root = tree.Add(style);
    tree.Compute(root, { 1000.0f, 1000.0f });
    CHECK_NEAR(tree.NodeRect(root).size.y, 300.0f);
}

// ------------------------------------------------------------------ alignment

TEST(layout, cross_axis_alignment_places_children) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row);
    rootStyle.width = Size::Px(200.0f);
    rootStyle.height = Size::Px(100.0f);
    const u32 root = tree.Add(rootStyle);
    tree.Add(Box(Size::Px(20.0f), Size::Px(40.0f)), root);

    const u32 child = tree.Children(root)[0];

    tree.Compute(root, { 500.0f, 500.0f });
    CHECK_NEAR(tree.NodeRect(child).pos.y, 0.0f);

    rootStyle.align = Align::Center;
    tree.SetStyle(root, rootStyle);
    tree.Compute(root, { 500.0f, 500.0f });
    CHECK_NEAR(tree.NodeRect(child).pos.y, 30.0f);

    rootStyle.align = Align::End;
    tree.SetStyle(root, rootStyle);
    tree.Compute(root, { 500.0f, 500.0f });
    CHECK_NEAR(tree.NodeRect(child).pos.y, 60.0f);

    // Stretch does NOT override an explicit size: a stated height outranks an alignment, the same
    // way CSS `align-items: stretch` leaves a non-auto cross size alone.
    rootStyle.align = Align::Stretch;
    tree.SetStyle(root, rootStyle);
    tree.Compute(root, { 500.0f, 500.0f });
    CHECK_NEAR(tree.NodeRect(child).size.y, 40.0f);
}

TEST(layout, stretch_expands_a_hugging_child_across_the_cross_axis) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row);
    rootStyle.width = Size::Px(200.0f);
    rootStyle.height = Size::Px(100.0f);
    rootStyle.align = Align::Stretch;
    const u32 root = tree.Add(rootStyle);
    const u32 child = tree.Add(Box(Size::Px(20.0f), Size::Hug()), root);
    tree.SetIntrinsic(child, { 0.0f, 12.0f });

    tree.Compute(root, { 500.0f, 500.0f });
    CHECK_NEAR(tree.NodeRect(child).size.y, 100.0f);
}

TEST(layout, justify_distributes_main_axis_slack) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row);
    rootStyle.width = Size::Px(300.0f);
    rootStyle.height = Size::Px(50.0f);
    const u32 root = tree.Add(rootStyle);
    tree.Add(Box(Size::Px(50.0f), Size::Px(10.0f)), root);
    tree.Add(Box(Size::Px(50.0f), Size::Px(10.0f)), root);
    const auto& kids = tree.Children(root);

    auto Recompute = [&](Justify justify) {
        rootStyle.justify = justify;
        tree.SetStyle(root, rootStyle);
        tree.Compute(root, { 500.0f, 500.0f });
    };

    Recompute(Justify::Start);
    CHECK_NEAR(tree.NodeRect(kids[0]).pos.x, 0.0f);

    Recompute(Justify::Center);              // 200 slack -> 100 leading
    CHECK_NEAR(tree.NodeRect(kids[0]).pos.x, 100.0f);

    Recompute(Justify::End);
    CHECK_NEAR(tree.NodeRect(kids[0]).pos.x, 200.0f);

    Recompute(Justify::SpaceBetween);        // first flush left, second flush right
    CHECK_NEAR(tree.NodeRect(kids[0]).pos.x, 0.0f);
    CHECK_NEAR(tree.NodeRect(kids[1]).pos.x, 250.0f);

    Recompute(Justify::SpaceEvenly);         // three equal gaps of 200/3
    CHECK_NEAR(tree.NodeRect(kids[0]).pos.x, 200.0f / 3.0f);
}

TEST(layout, justify_has_no_slack_to_distribute_when_a_child_fills) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row);
    rootStyle.width = Size::Px(300.0f);
    rootStyle.height = Size::Px(50.0f);
    rootStyle.justify = Justify::Center;
    const u32 root = tree.Add(rootStyle);
    tree.Add(Box(Size::Px(50.0f), Size::Px(10.0f)), root);
    tree.Add(Box(Size::Fill(), Size::Px(10.0f)), root);
    tree.Compute(root, { 500.0f, 500.0f });

    CHECK_NEAR(tree.NodeRect(tree.Children(root)[0]).pos.x, 0.0f);
    CHECK_NEAR(tree.NodeRect(tree.Children(root)[1]).size.x, 250.0f);
}

// ------------------------------------------------------------------ constraints

TEST(layout, absolute_children_use_their_offsets) {
    LayoutTree tree;
    const u32 root = tree.Add(Box(Size::Px(400.0f), Size::Px(300.0f)));
    LayoutStyle child = Box(Size::Px(50.0f), Size::Px(20.0f));
    child.offsetStart = { 30.0f, 40.0f };
    tree.Add(child, root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    const Rect r = tree.NodeRect(tree.Children(root)[0]);
    CHECK_NEAR(r.pos.x, 30.0f);
    CHECK_NEAR(r.pos.y, 40.0f);
}

TEST(layout, end_constraint_pins_to_the_far_edge_under_resize) {
    LayoutTree tree;
    LayoutStyle rootStyle = Box(Size::Fill(), Size::Fill());
    const u32 root = tree.Add(rootStyle);

    LayoutStyle child = Box(Size::Px(50.0f), Size::Px(20.0f));
    child.constraintX = Constraint::End;
    child.offsetEnd = { 10.0f, 0.0f };
    const u32 index = tree.Add(child, root);

    tree.Compute(root, { 400.0f, 300.0f });
    CHECK_NEAR(tree.NodeRect(index).pos.x, 340.0f);      // 400 - 10 - 50

    tree.Compute(root, { 800.0f, 300.0f });
    CHECK_NEAR(tree.NodeRect(index).pos.x, 740.0f);      // still 10 from the right edge
    CHECK_NEAR(tree.NodeRect(index).size.x, 50.0f);      // and still its own width
}

TEST(layout, both_edges_pinned_stretches_the_child) {
    LayoutTree tree;
    const u32 root = tree.Add(Box(Size::Fill(), Size::Fill()));

    LayoutStyle child = Box(Size::Px(50.0f), Size::Px(20.0f));
    child.constraintX = Constraint::StartEnd;
    child.offsetStart = { 20.0f, 0.0f };
    child.offsetEnd   = { 30.0f, 0.0f };
    const u32 index = tree.Add(child, root);

    tree.Compute(root, { 400.0f, 100.0f });
    CHECK_NEAR(tree.NodeRect(index).pos.x, 20.0f);
    CHECK_NEAR(tree.NodeRect(index).size.x, 350.0f);

    tree.Compute(root, { 600.0f, 100.0f });
    CHECK_NEAR(tree.NodeRect(index).size.x, 550.0f);     // grows with the parent
}

TEST(layout, a_filling_absolute_child_stops_at_the_parents_edge) {
    LayoutTree tree;
    const u32 root = tree.Add(Box(Size::Fill(), Size::Fill()));

    // A widget dropped on a canvas at x=640 and set to fill: the width it wants is what is left of
    // the parent, not the parent's whole width, or it hangs 640px off the right-hand side.
    LayoutStyle child = Box(Size::Fill(), Size::Px(20.0f));
    child.offsetStart = { 640.0f, 0.0f };
    const u32 index = tree.Add(child, root);

    tree.Compute(root, { 1280.0f, 800.0f });
    CHECK_NEAR(tree.NodeRect(index).pos.x, 640.0f);
    CHECK_NEAR(tree.NodeRect(index).size.x, 640.0f);
    CHECK_NEAR(tree.NodeRect(index).pos.x + tree.NodeRect(index).size.x, 1280.0f);

    // Pinned to the right instead, the offset that counts is the one from that edge.
    LayoutStyle right = Box(Size::Fill(), Size::Px(20.0f));
    right.constraintX = Constraint::End;
    right.offsetEnd = { 80.0f, 0.0f };
    const u32 pinned = tree.Add(right, root);

    tree.Compute(root, { 1280.0f, 800.0f });
    CHECK_NEAR(tree.NodeRect(pinned).size.x, 1200.0f);
    CHECK_NEAR(tree.NodeRect(pinned).pos.x, 0.0f);
}

TEST(layout, centre_constraint_keeps_its_offset_from_the_middle) {
    LayoutTree tree;
    const u32 root = tree.Add(Box(Size::Fill(), Size::Fill()));

    LayoutStyle child = Box(Size::Px(100.0f), Size::Px(20.0f));
    child.constraintX = Constraint::Center;
    child.offsetStart = { 25.0f, 0.0f };
    const u32 index = tree.Add(child, root);

    tree.Compute(root, { 400.0f, 100.0f });
    CHECK_NEAR(tree.NodeRect(index).pos.x, 175.0f);      // (400-100)/2 + 25

    tree.Compute(root, { 800.0f, 100.0f });
    CHECK_NEAR(tree.NodeRect(index).pos.x, 375.0f);      // (800-100)/2 + 25
}

TEST(layout, absolute_parent_hugs_the_far_edge_of_its_children) {
    LayoutTree tree;
    const u32 root = tree.Add(Box(Size::Hug(), Size::Hug()));

    LayoutStyle a = Box(Size::Px(40.0f), Size::Px(10.0f));
    a.offsetStart = { 100.0f, 0.0f };
    tree.Add(a, root);

    LayoutStyle b = Box(Size::Px(10.0f), Size::Px(60.0f));
    b.offsetStart = { 0.0f, 20.0f };
    tree.Add(b, root);

    tree.Compute(root, { 1000.0f, 1000.0f });
    CHECK_NEAR(tree.NodeRect(root).size.x, 140.0f);      // 100 + 40
    CHECK_NEAR(tree.NodeRect(root).size.y, 80.0f);       // 20 + 60
}

// ------------------------------------------------------------------ tree

TEST(layout, absolute_rect_accumulates_ancestor_offsets) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Column, 0.0f, Edges{ 10.0f });
    rootStyle.width = Size::Px(200.0f);
    rootStyle.height = Size::Px(200.0f);
    const u32 root = tree.Add(rootStyle);

    LayoutStyle midStyle = Stack(Axis::Column, 0.0f, Edges{ 5.0f });
    midStyle.width = Size::Px(100.0f);
    midStyle.height = Size::Px(100.0f);
    const u32 mid = tree.Add(midStyle, root);

    const u32 leaf = tree.Add(Box(Size::Px(10.0f), Size::Px(10.0f)), mid);
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(leaf).pos.x, 5.0f);          // parent space
    CHECK_NEAR(tree.AbsoluteRect(leaf).pos.x, 15.0f);     // + root's 10 padding
}

TEST(layout, deep_nesting_terminates_and_stays_consistent) {
    // 200 levels deep, each hugging: a solver that recursed per level per pass would show up here.
    LayoutTree tree;
    LayoutStyle style = Stack(Axis::Column, 0.0f, Edges{ 1.0f });
    u32 parent = tree.Add(style);
    const u32 root = parent;
    for (int i = 0; i < 200; ++i) parent = tree.Add(style, parent);
    tree.SetIntrinsic(parent, { 10.0f, 10.0f });

    tree.Compute(root, { 1000.0f, 1000.0f });
    // Each of the 201 levels adds 1px of padding on each side.
    CHECK_NEAR(tree.NodeRect(root).size.x, 10.0f + 2.0f * 201.0f);
    // The leaf is offset by its own parent's padding plus every ancestor's, but the root sits at
    // the origin and contributes none — so 200, not 201.
    CHECK_NEAR(tree.AbsoluteRect(parent).pos.x, 200.0f);
}

TEST(layout, text_intrinsic_size_drives_a_hugging_parent) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 6.0f, Edges{ 8.0f });
    const u32 root = tree.Add(rootStyle);
    const u32 label = tree.Add(Box(Size::Hug(), Size::Hug()), root);
    const u32 icon  = tree.Add(Box(Size::Px(16.0f), Size::Px(16.0f)), root);

    tree.SetIntrinsic(label, { 73.0f, 18.0f });   // as if measured by TextLayout
    tree.Compute(root, { 1000.0f, 1000.0f });

    CHECK_NEAR(tree.NodeRect(root).size.x, 73.0f + 6.0f + 16.0f + 16.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 18.0f + 16.0f);
    CHECK_NEAR(tree.NodeRect(icon).pos.x, 8.0f + 73.0f + 6.0f);
}

TEST(layout, recompute_is_idempotent) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 5.0f, Edges{ 4.0f });
    rootStyle.width = Size::Px(300.0f);
    const u32 root = tree.Add(rootStyle);
    tree.Add(Box(Size::Fill(), Size::Px(20.0f)), root);
    tree.Add(Box(Size::Px(40.0f), Size::Px(20.0f)), root);

    tree.Compute(root, { 500.0f, 500.0f });
    const Rect first = tree.NodeRect(tree.Children(root)[0]);
    tree.Compute(root, { 500.0f, 500.0f });
    tree.Compute(root, { 500.0f, 500.0f });
    const Rect third = tree.NodeRect(tree.Children(root)[0]);

    CHECK(first == third);
}

// ------------------------------------------------------------------ wrapping

TEST(layout, wrap_breaks_children_onto_new_lines) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 10.0f);
    rootStyle.width = Size::Px(220.0f);
    rootStyle.height = Size::Hug();
    rootStyle.wrap = true;
    const u32 root = tree.Add(rootStyle);

    // Four 100px chips with a 10px gap: two fit per 220px line.
    for (int i = 0; i < 4; ++i) tree.Add(Box(Size::Px(100.0f), Size::Px(30.0f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    const auto& kids = tree.Children(root);
    CHECK_NEAR(tree.NodeRect(kids[0]).pos.x, 0.0f);
    CHECK_NEAR(tree.NodeRect(kids[1]).pos.x, 110.0f);
    CHECK_NEAR(tree.NodeRect(kids[2]).pos.x, 0.0f);        // wrapped
    CHECK_NEAR(tree.NodeRect(kids[3]).pos.x, 110.0f);

    CHECK_NEAR(tree.NodeRect(kids[0]).pos.y, 0.0f);
    CHECK_NEAR(tree.NodeRect(kids[2]).pos.y, 40.0f);       // 30 tall + 10 gap
}

TEST(layout, a_wrapping_parent_hugs_to_the_total_height_of_its_lines) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 10.0f);
    rootStyle.width = Size::Px(220.0f);
    rootStyle.height = Size::Hug();
    rootStyle.wrap = true;
    const u32 root = tree.Add(rootStyle);
    for (int i = 0; i < 4; ++i) tree.Add(Box(Size::Px(100.0f), Size::Px(30.0f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    // Two lines of 30 plus one 10px gap between them. Hugging to a single line's height here would
    // leave the second row hanging outside the parent.
    CHECK_NEAR(tree.NodeRect(root).size.y, 70.0f);
}

TEST(layout, wrap_off_keeps_everything_on_one_line_even_when_it_overflows) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 10.0f);
    rootStyle.width = Size::Px(220.0f);
    rootStyle.wrap = false;
    const u32 root = tree.Add(rootStyle);
    for (int i = 0; i < 4; ++i) tree.Add(Box(Size::Px(100.0f), Size::Px(30.0f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    const auto& kids = tree.Children(root);
    CHECK_NEAR(tree.NodeRect(kids[3]).pos.x, 330.0f);      // runs past the parent, by design
    CHECK_NEAR(tree.NodeRect(kids[3]).pos.y, 0.0f);
}

TEST(layout, fill_children_divide_the_line_they_land_on) {
    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Row, 0.0f);
    rootStyle.width = Size::Px(300.0f);
    rootStyle.height = Size::Px(50.0f);
    rootStyle.wrap = true;
    const u32 root = tree.Add(rootStyle);
    tree.Add(Box(Size::Fill(), Size::Px(20.0f)), root);
    tree.Add(Box(Size::Fill(), Size::Px(20.0f)), root);
    tree.Compute(root, { 1000.0f, 1000.0f });

    const auto& kids = tree.Children(root);
    CHECK_NEAR(tree.NodeRect(kids[0]).size.x, 150.0f);
    CHECK_NEAR(tree.NodeRect(kids[1]).size.x, 150.0f);
}


// ------------------------------------------------------------------ grid

TEST(layout, a_grid_lays_children_out_in_equal_columns) {
    LayoutTree tree;
    const u32 root = tree.Add(Grid(3, 10.0f));
    tree.SetStyle(root, [&] {
        LayoutStyle s = Grid(3, 10.0f);
        s.width = Size::Px(320.0f);
        s.height = Size::Px(200.0f);
        return s;
    }());

    std::vector<u32> cells;
    for (int i = 0; i < 5; ++i) {
        const u32 cell = tree.Add(Box(Size::Fill(), Size::Px(40.0f)), root);
        cells.push_back(cell);
    }
    tree.Compute(root, { 320.0f, 200.0f });

    // 320 across, two 10px gaps, three tracks: 100 each.
    for (const u32 cell : cells) CHECK_NEAR(tree.NodeRect(cell).size.x, 100.0f);

    CHECK_NEAR(tree.AbsoluteRect(cells[0]).pos.x, 0.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[1]).pos.x, 110.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[2]).pos.x, 220.0f);
    // The fourth wraps onto a second row rather than a fourth column.
    CHECK_NEAR(tree.AbsoluteRect(cells[3]).pos.x, 0.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[3]).pos.y, 50.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[4]).pos.x, 110.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[4]).pos.y, 50.0f);
}

TEST(layout, a_row_is_as_tall_as_its_tallest_cell) {
    LayoutTree tree;
    LayoutStyle style = Grid(2, 8.0f);
    style.width = Size::Px(200.0f);
    const u32 root = tree.Add(style);

    const u32 shortCell = tree.Add(Box(Size::Fill(), Size::Px(30.0f)), root);
    const u32 tallCell  = tree.Add(Box(Size::Fill(), Size::Px(80.0f)), root);
    const u32 next      = tree.Add(Box(Size::Fill(), Size::Px(20.0f)), root);
    tree.Compute(root, { 200.0f, kUnbounded });

    // Cells keep the height they asked for; the row they share does not.
    CHECK_NEAR(tree.NodeRect(shortCell).size.y, 30.0f);
    CHECK_NEAR(tree.NodeRect(tallCell).size.y, 80.0f);
    // The second row starts below the taller of the two, plus the gap.
    CHECK_NEAR(tree.AbsoluteRect(next).pos.y, 88.0f);
    // And the grid hugs to both rows.
    CHECK_NEAR(tree.NodeRect(root).size.y, 108.0f);
}

TEST(layout, a_grid_with_no_column_count_fits_as_many_as_it_can) {
    // The version that survives a resize: a gallery reflows rather than overflowing.
    LayoutStyle style;
    style.mode = LayoutMode::Grid;
    style.columns = 0;
    style.minColumn = 100.0f;
    style.gap = 10.0f;
    style.width = Size::Px(340.0f);

    LayoutTree tree;
    const u32 root = tree.Add(style);
    std::vector<u32> cells;
    for (int i = 0; i < 6; ++i) cells.push_back(tree.Add(Box(Size::Fill(), Size::Px(20.0f)), root));
    tree.Compute(root, { 340.0f, kUnbounded });

    // 340 fits three 100px minimums with two gaps, so three columns — and they then share the
    // leftover rather than staying at the minimum, which is what makes the row reach both edges.
    CHECK_NEAR(tree.AbsoluteRect(cells[2]).pos.y, 0.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[3]).pos.y, 30.0f);
    CHECK_NEAR(tree.NodeRect(cells[0]).size.x, 320.0f / 3.0f);

    // Narrower, and the same content is two columns without anyone editing it.
    tree.Compute(root, { 220.0f, kUnbounded });
    LayoutStyle narrow = style;
    narrow.width = Size::Px(220.0f);
    tree.SetStyle(root, narrow);
    tree.Compute(root, { 220.0f, kUnbounded });
    CHECK_NEAR(tree.AbsoluteRect(cells[1]).pos.y, 0.0f);
    CHECK_NEAR(tree.AbsoluteRect(cells[2]).pos.y, 30.0f);
    CHECK_NEAR(tree.NodeRect(cells[0]).size.x, 105.0f);
    // Never fewer than one, even in a container narrower than a single column.
    LayoutStyle tiny = narrow;
    tiny.width = Size::Px(40.0f);
    tree.SetStyle(root, tiny);
    tree.Compute(root, { 40.0f, kUnbounded });
    CHECK_NEAR(tree.AbsoluteRect(cells[1]).pos.y, 30.0f);
}

TEST(layout, a_cell_smaller_than_its_track_is_placed_in_it) {
    LayoutStyle style = Grid(2, 0.0f);
    style.width = Size::Px(200.0f);
    style.justify = Justify::Center;
    style.align = Align::Center;

    LayoutTree tree;
    const u32 root = tree.Add(style);
    const u32 small = tree.Add(Box(Size::Px(40.0f), Size::Px(20.0f)), root);
    tree.Add(Box(Size::Fill(), Size::Px(60.0f)), root);
    tree.Compute(root, { 200.0f, kUnbounded });

    // A 40px cell in a 100px track, centred; 20px tall in a 60px row, centred.
    CHECK_NEAR(tree.AbsoluteRect(small).pos.x, 30.0f);
    CHECK_NEAR(tree.AbsoluteRect(small).pos.y, 20.0f);
}

TEST(layout, an_aspect_ratio_applies_to_a_box_that_only_gets_its_width_when_arranged) {
    // The node an aspect ratio is actually for: one that fills its parent. Measure cannot help —
    // "whatever is left" is not known until the parent divides it — so the ratio has to survive
    // into arrange, or a 16:9 frame is a 16:0 frame.
    LayoutTree tree;
    LayoutStyle row = Stack(Axis::Row, 0.0f);
    row.width = Size::Px(320.0f);
    row.height = Size::Px(400.0f);
    const u32 root = tree.Add(row);

    LayoutStyle media = Box(Size::Fill(), Size::Hug());
    media.aspectRatio = 16.0f / 9.0f;
    const u32 frame = tree.Add(media, root);
    tree.Compute(root, { 320.0f, 400.0f });

    CHECK_NEAR(tree.NodeRect(frame).size.x, 320.0f);
    CHECK_NEAR(tree.NodeRect(frame).size.y, 320.0f * 9.0f / 16.0f);
}

TEST(layout, a_stated_size_outranks_an_aspect_ratio) {
    LayoutTree tree;
    LayoutStyle style = Box(Size::Px(200.0f), Size::Px(200.0f));
    style.aspectRatio = 2.0f;
    const u32 root = tree.Add(style);
    tree.Compute(root, { 500.0f, 500.0f });
    CHECK_NEAR(tree.NodeRect(root).size.x, 200.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 200.0f);
}

TEST(layout, a_grid_that_ends_up_narrower_than_it_was_measured_reflows_and_grows_its_parent) {
    // Measure offers a Fill child the whole row; arrange gives it only what is left. A grid that
    // reflows in the gap between those two answers used to leave its rows hanging outside a parent
    // that hugged the wrong height — the same two-pass problem wrapped text already had.
    LayoutStyle grid;
    grid.mode = LayoutMode::Grid;
    grid.columns = 0;
    grid.minColumn = 160.0f;
    grid.gap = 12.0f;
    grid.width = Size::Fill();
    grid.height = Size::Hug();

    LayoutTree tree;
    const u32 root = tree.Add(Stack(Axis::Row, 0.0f));
    tree.SetStyle(root, [&] { LayoutStyle s = Stack(Axis::Row, 0.0f);
                              s.width = Size::Px(600.0f); s.height = Size::Hug(); return s; }());
    const u32 cells = tree.Add(grid, root);
    for (int i = 0; i < 6; ++i) tree.Add(Box(Size::Fill(), Size::Px(88.0f)), cells);
    tree.Add(Box(Size::Px(300.0f), Size::Px(20.0f)), root);

    tree.Compute(root, { 600.0f, kUnbounded });

    // 300px left over is one 160px column, so six rows: 6 * 88 + 5 * 12.
    CHECK_NEAR(tree.NodeRect(cells).size.x, 300.0f);
    CHECK_NEAR(tree.NodeRect(cells).size.y, 588.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 588.0f);
}

TEST(layout, an_aspect_ratio_child_of_a_hugging_stack_reserves_the_height_it_will_take) {
    // The same two-pass problem as a reflowing grid, from the other direction: a Fill-width node
    // whose height comes from its width measures at whatever it can guess and then arranges at the
    // width it was given. The parent hugged the guess, so the next sibling was drawn on top of it.
    LayoutStyle banner;
    banner.width = Size::Fill();
    banner.height = Size::Hug();
    banner.aspectRatio = 3.0f;

    LayoutTree tree;
    LayoutStyle rootStyle = Stack(Axis::Column, 0.0f);
    rootStyle.width = Size::Px(600.0f);
    rootStyle.height = Size::Hug();

    const u32 root = tree.Add(rootStyle);
    const u32 chart = tree.Add(banner, root);
    const u32 after = tree.Add(Box(Size::Px(600.0f), Size::Px(20.0f)), root);
    tree.Compute(root, { 600.0f, kUnbounded });

    CHECK_NEAR(tree.NodeRect(chart).size.x, 600.0f);
    CHECK_NEAR(tree.NodeRect(chart).size.y, 200.0f);
    CHECK_NEAR(tree.NodeRect(root).size.y, 220.0f);
    // And what follows it starts below it rather than on top of it.
    CHECK_NEAR(tree.NodeRect(after).pos.y, 200.0f);
}
