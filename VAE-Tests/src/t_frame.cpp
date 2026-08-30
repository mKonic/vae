#include "Test.h"

#include "vae/base/FileSystem.h"
#include "vae/draw/DrawList.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextCache.h"
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"

#include <chrono>
#include <string>

using namespace vae;
using namespace vae::ui;

// What the frame costs, guarded.
//
// Two wins the audit measured are load-bearing and were guarded by nothing: the text memo (38 ms →
// 3.7 ms on a 2,000-row list) and paint culling (4,002 views → 16 draw commands). Both are the kind
// of thing a refactor deletes without breaking a single behaviour test — the app still looks right,
// it is just twenty times slower, and nothing says so.
//
// Almost none of this is a stopwatch. A wall-clock ceiling on a shared machine is a test that fails
// for reasons that have nothing to do with the code, so what is asserted here is *work done*: cache
// misses, draw commands, whether a solve ran. The one timing check left is a backstop three orders
// of magnitude above the measurement, which catches an accidental O(n²) and nothing else.
namespace {

    // A list of `rows` copies of one authored row — the shape every list in VAE is, and the shape
    // both wins were measured on.
    struct Rows {
        doc::Document document;
        Library library;
        UiHost host;
        Uuid screen, list, row, label;
        Vec2 size{ 1200.0f, 800.0f };

        // `clip` is what decides whether the list is virtualized: a container something clips has
        // a box to window against, and one nothing clips is as tall as it is long. Both are real
        // cases and the tests below want one each.
        explicit Rows(int rows, bool distinctText = false, bool clip = true) {
            static const bool fonts = [] {
                text::FontDB::Get().RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"),
                                                      true, true);
                text::FontDB::Get().SetDefaultFamily("JetBrainsMono Nerd Font");
                return true;
            }();
            (void)fonts;

            library = BuildStandardLibrary(document);
            screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Bench");
            doc::Node* top = document.Find(screen);
            top->layout.mode = layout::LayoutMode::Stack;
            top->layout.axis = layout::Axis::Column;
            top->layout.width = layout::Size::Px(size.x);
            top->layout.height = layout::Size::Px(size.y);

            list = document.CreateNode(doc::NodeKind::Frame, screen, "List");
            doc::Node* box = document.Find(list);
            box->layout.mode = layout::LayoutMode::Stack;
            box->layout.axis = layout::Axis::Column;
            box->layout.width = layout::Size::Fill();
            box->layout.height = layout::Size::Fill();
            box->layout.gap = 4.0f;
            box->props.Set(doc::Prop::Repeat, static_cast<f32>(rows));
            box->props.Set(doc::Prop::ClipContent, clip);

            row = document.CreateNode(doc::NodeKind::Frame, list, "Row");
            doc::Node* item = document.Find(row);
            item->layout.mode = layout::LayoutMode::Stack;
            item->layout.axis = layout::Axis::Row;
            item->layout.width = layout::Size::Fill();
            item->layout.height = layout::Size::Px(48.0f);
            item->layout.padding = Edges(8.0f);
            item->props.Set(doc::Prop::Fill, doc::TokenRef{ "surface" });
            item->props.Set(doc::Prop::Role, std::string("Button"));
            item->props.Set(std::string("hovered:fill"), doc::TokenRef{ "accent" });

            label = document.CreateNode(doc::NodeKind::Text, row, "Label");
            doc::Node* text = document.Find(label);
            text->layout.width = layout::Size::Fill();
            text->props.Set(doc::Prop::FontSize, 15.0f);
            // Every row saying the same thing is one shaped run; every row saying something else is
            // `rows` of them. Which of those a list is decides what the memo is worth.
            if (distinctText) {
                std::string table = "body\n";
                for (int i = 0; i < rows; ++i) table += "message number " + std::to_string(i) + "\n";
                box->props.Set(doc::Prop::Sample, table);
                text->props.Set(doc::Prop::Field, std::string("body"));
            }
            text->props.Set(doc::Prop::Text, std::string("A message body of ordinary length"));

            host.SetDocument(document, screen);
            host.Tree().ShowSampleRows(distinctText);
        }

        void Frame() { host.Update(size, 1.0f / 60.0f); host.ClearActions(); }
    };

    u64 Misses() { return text::TextCache::Report().misses; }

}

TEST(frame, a_thousand_identical_labels_are_shaped_once) {
    // The memo, asserted as work rather than as elapsed time. Without it every row measures its own
    // copy of the same sentence, which is what the 38 ms was.
    text::TextCache::Clear();
    const u64 before = Misses();
    Rows app(1000, false, false);                         // unclipped, so every row is built
    app.Frame();

    CHECK(app.host.Tree().ViewCount() > 2000u);           // the rows really were flattened
    const u64 shaped = Misses() - before;
    // One run per (text, face, size, width). A handful, not a thousand — the width a row is
    // measured at can legitimately differ once or twice between the hug pass and the arrange pass.
    CHECK_MESSAGE(shaped < 20, "shaped " + std::to_string(shaped) + " runs for 1000 identical labels");
}

TEST(frame, a_thousand_different_labels_are_shaped_a_thousand_times) {
    // The other side of it, so the test above cannot pass by the memo returning a stale answer for
    // text that actually differs.
    text::TextCache::Clear();
    const u64 before = Misses();
    Rows app(1000, true, false);
    app.Frame();

    const u64 shaped = Misses() - before;
    CHECK_MESSAGE(shaped > 500, "shaped only " + std::to_string(shaped) + " runs for 1000 distinct labels");
}

TEST(frame, only_what_is_on_screen_is_painted) {
    // Culling, on a list short enough that nothing windows it — the two mechanisms are separate
    // and this is the one that has to work for a list built whole. Three hundred views, a screen
    // that can show sixteen rows, and a draw list the size of what is visible.
    Rows app(150);
    app.Frame();
    CHECK(app.host.Tree().ViewCount() > 300u);

    draw::DrawList list;
    PaintContext paint;
    paint.list = &list;
    app.host.Paint(paint);

    CHECK(!list.Quads().empty());                          // it did draw something
    CHECK_MESSAGE(list.Quads().size() < 200,
                  std::to_string(list.Quads().size()) + " quads for a screen that holds ~16 rows");
}

TEST(frame, a_long_list_builds_a_window_rather_than_a_document) {
    // The whole of L1. Fifty thousand rows used to be impossible — the flatten capped every list
    // at two thousand and said so in a warning. Now the list is as long as it says it is, and the
    // tree holds the rows the box can show plus a margin either side.
    Rows app(50000);
    app.Frame();
    app.Frame();   // the first frame measures a row; the second windows against it

    const u32 views = app.host.Tree().ViewCount();
    CHECK_MESSAGE(views < 400, std::to_string(views) + " views for a 50,000-row list");
    CHECK(views > 20);   // and it did build the rows that are actually on screen
}

TEST(frame, a_windowed_list_is_still_as_tall_as_it_is_long) {
    // The half that is easy to get wrong. Building thirty rows out of fifty thousand is only
    // correct if the container is still fifty thousand rows tall — otherwise the scrollbar lies,
    // the offset means nothing, and scrolling stops at the bottom of the window.
    Rows app(50000);
    app.Frame();
    app.Frame();

    const ui::ViewTree& tree = app.host.Tree();
    u32 list = ui::ViewTree::kInvalid;
    for (u32 i = 0; i < tree.ViewCount(); ++i)
        if (tree.At(i).sourceId == app.list) { list = i; break; }
    CHECK(list != ui::ViewTree::kInvalid);

    // Its *content*, not its box: this list is the scroller, so its box is the viewport and the
    // length of the list is what a scrollbar would be drawn from. 48 per row plus the 4 gap, fifty
    // thousand times, less the last gap.
    const f32 content = tree.ContentSize(list).y;
    CHECK_MESSAGE(content > 2'000'000.0f, std::to_string(content) + " of content for 50,000 rows");
    CHECK_NEAR(content, 50000.0f * 52.0f - 4.0f);
    // And the box really is only a screenful, so the rows were not all laid out somewhere.
    CHECK_NEAR(tree.Bounds(list).size.y, 800.0f);
}

TEST(frame, scrolling_a_long_list_brings_the_rows_it_reaches) {
    // A window that never moves is just a shorter list. Scrolling past the rows that were built
    // has to build the ones it lands on — and the copies keep the identity their widget state is
    // filed under, which is why the row index travels rather than the position in the window.
    Rows app(50000);
    app.Frame();
    app.Frame();

    const auto rowsOnScreen = [&app] {
        std::vector<i32> out;
        const ui::ViewTree& tree = app.host.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).rowRoot) out.push_back(tree.At(i).row);
        return out;
    };

    const std::vector<i32> before = rowsOnScreen();
    CHECK(!before.empty());
    CHECK_EQ(before.front(), 0);

    // A long way down: row 20,000 of 50,000.
    u32 list = ui::ViewTree::kInvalid;
    const ui::ViewTree& tree = app.host.Tree();
    for (u32 i = 0; i < tree.ViewCount(); ++i)
        if (tree.At(i).sourceId == app.list) { list = i; break; }
    app.host.Tree().SetScroll(list, { 0.0f, 20000.0f * 52.0f });
    app.Frame();
    app.Frame();

    const std::vector<i32> after = rowsOnScreen();
    CHECK(!after.empty());
    CHECK_MESSAGE(after.front() > 19000, "first row on screen is " + std::to_string(after.front()));
    CHECK_MESSAGE(after.front() < 21000, "first row on screen is " + std::to_string(after.front()));
    CHECK(after.size() < 400);
}

TEST(frame, a_row_sits_exactly_where_its_index_says_however_the_window_moved) {
    // The arithmetic that makes virtualization invisible. A spacer stands where the copies above
    // the window would have been, and a stack puts a gap after it — so the spacer is one gap
    // shorter than the space it stands for. Get that wrong by one gap and every row below sits
    // four pixels off, which reads as the list jumping each time the window slides.
    Rows app(50000);
    app.Frame();
    app.Frame();

    const ui::ViewTree& tree = app.host.Tree();
    u32 list = ui::ViewTree::kInvalid;
    for (u32 i = 0; i < tree.ViewCount(); ++i)
        if (tree.At(i).sourceId == app.list) { list = i; break; }
    CHECK(list != ui::ViewTree::kInvalid);

    constexpr f32 kRow = 48.0f + 4.0f;   // the fixture's row height plus its gap
    const auto checkRows = [&](const char* when) {
        const ui::ViewTree& views = app.host.Tree();
        const f32 top = views.Bounds(list).pos.y - views.ScrollOffset(list).y;
        u32 checked = 0;
        for (u32 i = 0; i < views.ViewCount(); ++i) {
            if (!views.At(i).rowRoot) continue;
            const f32 want = top + static_cast<f32>(views.At(i).row) * kRow;
            const f32 got = views.Bounds(i).pos.y;
            CHECK_MESSAGE(std::abs(got - want) < 0.5f,
                          std::string(when) + ": row " + std::to_string(views.At(i).row)
                          + " is at " + std::to_string(got) + ", not " + std::to_string(want));
            ++checked;
        }
        CHECK(checked > 4);
    };

    checkRows("at the top");

    // Every offset a scroll can land on: exactly on a row, halfway through one, and far enough
    // that the window has been rebuilt several times over.
    for (const f32 offset : { 26.0f, 52.0f, 1000.0f, 1026.0f, 500000.0f, 500026.0f }) {
        app.host.Tree().SetScroll(list, { 0.0f, offset });
        app.Frame();
        app.Frame();
        checkRows("scrolled");
    }
}

TEST(frame, a_short_list_is_built_whole_and_pays_for_none_of_this) {
    // The threshold, asserted rather than assumed: every design that already worked keeps behaving
    // exactly as it did, and a list of thirty rows has thirty rows in the tree.
    Rows app(30);
    app.Frame();
    app.Frame();

    u32 rows = 0;
    const ui::ViewTree& tree = app.host.Tree();
    for (u32 i = 0; i < tree.ViewCount(); ++i) if (tree.At(i).rowRoot) ++rows;
    CHECK_EQ(rows, 30u);
}

TEST(frame, a_settled_screen_asks_for_no_more_frames) {
    // The idle gate. Everything that changes the picture says so through NeedsFrame; a screen that
    // nothing has touched must not, or the editor and the player both spin at vsync forever.
    Rows app(200);
    for (int i = 0; i < 4; ++i) app.Frame();
    CHECK(!app.host.NeedsFrame());
}

TEST(frame, laying_out_again_at_the_same_size_does_nothing) {
    // The solve gate. The layout answer is already in the tree, and recomputing it every frame at
    // 4,000 views was the whole of P1.
    Rows app(2000);
    app.Frame();

    // The solver's own count, not the dirty flag: Layout clears the flag on its way past, so a
    // gate that had been deleted would still leave it false and the test would prove nothing.
    const u64 solves = app.host.Tree().Solves();
    app.host.Tree().Layout(app.size);
    app.Frame();
    app.Frame();
    CHECK_EQ(app.host.Tree().Solves(), solves);

    // A gate that answered nothing at all would pass both checks above just as well, so: an edit
    // to the document is a reason to solve again, and the answer really does move.
    const u32 firstRow = app.host.Tree().Root();
    const f32 was = app.host.Tree().Bounds(firstRow).size.y;
    app.document.Find(app.row)->layout.height = layout::Size::Px(96.0f);
    app.document.Touch(app.row);
    app.Frame();
    CHECK(app.host.Tree().Solves() > solves);
    CHECK_EQ(app.host.Tree().Bounds(firstRow).size.y, was);   // the screen is a stated size
    // ...but the rows inside it are twice as tall as they were.
    bool taller = false;
    for (u32 v = 0; v < app.host.Tree().ViewCount(); ++v)
        if (app.host.Tree().Bounds(v).size.y > 90.0f) { taller = true; break; }
    CHECK(taller);
}

TEST(frame, a_steady_frame_at_four_thousand_views_is_not_quadratic) {
    // The one stopwatch, and it is a backstop rather than a budget: the measurement is ~0.03 ms and
    // the ceiling is 50, so this fails when someone makes the frame path scale with the document
    // instead of with the screen, and never because the machine was busy.
    Rows app(2000);
    app.Frame();

    draw::DrawList list;
    PaintContext paint;
    paint.list = &list;

    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 20;
    for (int i = 0; i < kFrames; ++i) {
        app.Frame();
        list.Reset();
        app.host.Paint(paint);
    }
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() / kFrames;

    CHECK_MESSAGE(ms < 50.0, std::to_string(ms) + " ms per steady frame at "
                             + std::to_string(app.host.Tree().ViewCount()) + " views");
}
