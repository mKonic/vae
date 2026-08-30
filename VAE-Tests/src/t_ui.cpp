#include "Test.h"

#include <cmath>
#include <cstring>

#include "vae/base/FileSystem.h"
#include "vae/base/Utf8.h"
#include "vae/ui/Library.h"
#include "vae/doc/Serializer.h"
#include "vae/text/FontDB.h"
#include "vae/ui/UiHost.h"

#include <algorithm>
#include <string>

using namespace vae;
using namespace vae::ui;

namespace {

    // A whole UI, headless: real components, real layout, real hit-testing, no device and no
    // window. Every interaction contract below is driven exactly the way the desktop backend
    // drives it — through vae::Event.
    struct Ui {
        doc::Document document;
        Library library;
        UiHost host;
        Uuid screen;
        Vec2 size{ 800.0f, 600.0f };

        Ui() {
            // Real text metrics, from the bundled face. Without a font every Text node measures
            // zero and a whole class of layout answer — a hugging paragraph, a caret on the second
            // line, a label that pushes its row wider — is untestable and quietly wrong.
            static const bool fonts = [] {
                text::FontDB::Get().RegisterDirectory(FileSystem::Asset("VAE/assets/fonts"),
                                                      true, true);
                text::FontDB::Get().SetDefaultFamily("JetBrainsMono Nerd Font");
                return true;
            }();
            (void)fonts;

            library = BuildStandardLibrary(document);
            screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Screen");
            doc::Node* node = document.Find(screen);
            node->layout.mode = layout::LayoutMode::Absolute;
            node->layout.width = layout::Size::Px(size.x);
            node->layout.height = layout::Size::Px(size.y);
            host.SetDocument(document, screen);
        }

        Uuid Place(std::string_view widget, Vec2 at = { 40.0f, 40.0f }) {
            const Uuid instance = document.CreateInstance(library.Find(widget), screen);
            doc::Node* node = document.Find(instance);
            node->layout.offsetStart = at;
            document.Touch(instance);
            Frame();
            return instance;
        }

        // A plain node on the screen, not an instance of anything: the case where a property
        // rather than a component is what makes a widget.
        Uuid Paragraph(std::string body, Vec2 at = { 40.0f, 40.0f }, f32 width = 320.0f) {
            const Uuid id = document.CreateNode(doc::NodeKind::Text, screen, "Paragraph");
            doc::Node* node = document.Find(id);
            node->layout.offsetStart = at;
            node->layout.width = layout::Size::Px(width);
            node->layout.height = layout::Size::Hug();
            node->layout.minSize.y = 20.0f;
            document.SetProp(id, doc::Prop::Text, std::move(body));
            document.SetProp(id, doc::Prop::FontSize, 13.0f);
            document.SetProp(id, doc::Prop::TextWrap, std::string("word"));
            document.SetProp(id, doc::Prop::Selectable, true);
            Frame();
            return id;
        }
        static WidgetId Plain(Uuid node) { return WidgetId{ node, Uuid::Invalid() }; }
        u32 ViewOfPlain(Uuid node) const { return host.Tree().ViewOf(Plain(node)); }
        Rect BoundsOfPlain(Uuid node) const { return host.Tree().Bounds(ViewOfPlain(node)); }

        void Frame(f32 dt = 1.0f / 60.0f) { host.Update(size, dt); }

        // Runs the transitions out. State changes ease now, so a test that asks "what colour is it
        // after hovering" has to mean "after it has finished becoming that colour".
        void Settle(int limit = 400) {
            // One frame first: a state change made by a dispatch has started a transition that no
            // Update has seen yet, so `Animating()` still reports the last frame's answer.
            Frame();
            for (int i = 0; i < limit && host.Animating(); ++i) Frame();
            Frame();   // and one more, which drops the arrived track so reads fall back to static
        }

        u32 View(Uuid instance) const {
            return host.Tree().ViewOf(WidgetId{ ComponentRoot(instance), instance });
        }
        Uuid ComponentRoot(Uuid instance) const {
            const doc::Node* node = document.Find(instance);
            return node ? node->componentId : Uuid::Invalid();
        }
        Rect Bounds(Uuid instance) const { return host.Tree().Bounds(View(instance)); }
        Vec2 Center(Uuid instance) const { return Bounds(instance).Center(); }

        f32 Number(Uuid instance, doc::Prop prop) const {
            return host.Tree().Number(View(instance), prop, -12345.0f);
        }
        bool Flag(Uuid instance, doc::Prop prop) const {
            return host.Tree().Flag(View(instance), prop);
        }
        std::string Str(Uuid instance, doc::Prop prop) const {
            return host.Tree().Str(View(instance), prop);
        }
        StateMask State(Uuid instance) const { return host.Tree().At(View(instance)).state; }

        void Move(Vec2 point) { host.Dispatch(MakeMouseMoved(point.x, point.y)); }
        void Press(Vec2 point) {
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
        }
        void Release(Vec2 point) {
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
        }
        void RightPress(Vec2 point) {
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Right,
                                          point.x, point.y, Mod::None));
        }
        void Click(Vec2 point) { Move(point); Press(point); Release(point); }
        void ClickOn(Uuid instance) { Click(Center(instance)); }
        void Wheel(f32 dy, u16 mods = Mod::None) { host.Dispatch(MakeScroll(0.0f, dy, mods)); }

        void Key(vae::Key code, u16 mods = Mod::None) {
            host.Dispatch(MakeKey(EventType::KeyPressed, code, mods));
        }
        void Type(std::string_view text) {
            std::size_t index = 0;
            while (index < text.size()) host.Dispatch(MakeTextInput(Utf8Next(text, index)));
        }

        std::vector<Action> Drain() { return host.TakeActions(); }
        bool Fired(ActionKind kind) const { return host.Fired(kind); }
        u32 CountOf(ActionKind kind) const {
            return static_cast<u32>(std::count_if(host.Actions().begin(), host.Actions().end(),
                                                  [&](const Action& a) { return a.kind == kind; }));
        }
    };

    // The text a widget shows: the same rule the widgets themselves use.
    u32 LabelIn(const Ui& ui, u32 view) {
        const ViewTree& tree = ui.host.Tree();
        std::vector<u32> stack{ view };
        while (!stack.empty()) {
            const u32 current = stack.back();
            stack.pop_back();
            const ViewTree::View& node = tree.At(current);
            if (current != view && node.role == Role::Content) continue;
            if (node.kind == doc::NodeKind::Text && node.name == "Label") return current;
            for (u32 child : node.children) stack.push_back(child);
        }
        return ViewTree::kInvalid;
    }

}

// ------------------------------------------------------------------ state overlays

// ---------------------------------------------------------------------- breakpoints

namespace {

    // A shell that fills the screen, with a fixed sidebar and a filling body. The layout every app
    // has, and the one a narrow window has to rearrange.
    struct Shell {
        Ui ui;
        Uuid shell, sidebar, body;

        explicit Shell(f32 screenWidth = 1400.0f) {
            ui.size = { screenWidth, 800.0f };
            doc::Node* screen = ui.document.Find(ui.screen);
            screen->layout.mode = layout::LayoutMode::Stack;
            screen->layout.width = layout::Size::Px(screenWidth);
            screen->layout.height = layout::Size::Px(800.0f);

            shell = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Shell");
            doc::Node* s = ui.document.Find(shell);
            s->layout.mode = layout::LayoutMode::Stack;
            s->layout.axis = layout::Axis::Row;
            s->layout.width = layout::Size::Fill();
            s->layout.height = layout::Size::Fill();
            s->layout.gap = 24.0f;
            s->layout.padding = Edges(32.0f);

            // Both children are rows too, so "did this one turn into a column?" is a question
            // each of them can answer differently — which is the whole of the container-query
            // test below. A default-Column child could not tell the two answers apart.
            sidebar = ui.document.CreateNode(doc::NodeKind::Frame, shell, "Sidebar");
            doc::Node* side = ui.document.Find(sidebar);
            side->layout.mode = layout::LayoutMode::Stack;
            side->layout.axis = layout::Axis::Row;
            side->layout.width = layout::Size::Px(280.0f);
            side->layout.height = layout::Size::Fill();

            body = ui.document.CreateNode(doc::NodeKind::Frame, shell, "Body");
            doc::Node* b = ui.document.Find(body);
            b->layout.mode = layout::LayoutMode::Stack;
            b->layout.axis = layout::Axis::Row;
            b->layout.width = layout::Size::Fill();
            b->layout.height = layout::Size::Fill();
            ui.Frame();
        }

        void Width(f32 w) {
            ui.size = { w, 800.0f };
            ui.document.Find(ui.screen)->layout.width = layout::Size::Px(w);
            ui.document.Touch(ui.screen);
            ui.Frame();
        }
        const layout::LayoutStyle& StyleOf(Uuid node) const {
            return ui.host.Tree().LayoutStyleOf(ui.ViewOfPlain(node));
        }
    };

}

TEST(breakpoints, a_row_becomes_a_column_when_its_own_box_gets_narrow) {
    // The entire point. `axis` is a LayoutStyle field, not a property, so this is the thing no
    // overlay could reach before doc::LayoutFields existed.
    Shell app;
    app.ui.document.SetProp(app.shell, BreakpointKey("compact", "axis"), std::string("column"));
    app.ui.document.SetProp(app.shell, BreakpointKey("compact", "gap"), std::string("8"));
    app.ui.document.Touch(app.shell);
    app.ui.Frame();

    CHECK(app.StyleOf(app.shell).axis == layout::Axis::Row);
    CHECK_EQ(app.StyleOf(app.shell).gap, 24.0f);

    app.Width(500.0f);
    CHECK(app.StyleOf(app.shell).axis == layout::Axis::Column);
    CHECK_EQ(app.StyleOf(app.shell).gap, 8.0f);

    // And back: the overlay is recomputed from the authored style every layout, never stacked on
    // last frame's answer.
    app.Width(1400.0f);
    CHECK(app.StyleOf(app.shell).axis == layout::Axis::Row);
    CHECK_EQ(app.StyleOf(app.shell).gap, 24.0f);
}

TEST(breakpoints, the_narrowest_match_wins) {
    Shell app;
    app.ui.document.SetProp(app.shell, BreakpointKey("medium", "padding"), std::string("16"));
    app.ui.document.SetProp(app.shell, BreakpointKey("compact", "padding"), std::string("8"));
    app.ui.document.Touch(app.shell);
    app.ui.Frame();

    CHECK_EQ(app.StyleOf(app.shell).padding.left, 32.0f);   // wider than both
    app.Width(900.0f);
    CHECK_EQ(app.StyleOf(app.shell).padding.left, 16.0f);   // medium only
    app.Width(500.0f);
    CHECK_EQ(app.StyleOf(app.shell).padding.left, 8.0f);    // both match, compact is narrower
}

TEST(breakpoints, a_property_answers_to_a_width_the_same_way_a_layout_field_does) {
    Shell app;
    app.ui.document.SetProp(app.body, doc::Prop::FontSize, 18.0f);
    app.ui.document.SetProp(app.body, BreakpointKey("compact", doc::Prop::FontSize), 12.0f);
    app.ui.document.Touch(app.body);
    app.ui.Frame();

    const u32 view = app.ui.ViewOfPlain(app.body);
    CHECK_EQ(app.ui.host.Tree().Number(view, doc::Prop::FontSize, 0.0f), 18.0f);
    app.Width(500.0f);
    CHECK_EQ(app.ui.host.Tree().Number(app.ui.ViewOfPlain(app.body), doc::Prop::FontSize, 0.0f), 12.0f);
}

TEST(breakpoints, it_is_the_nodes_own_box_that_answers_not_the_window) {
    // A container query. The window is wide; the sidebar is 280 and is therefore compact, and the
    // body beside it is not. A media query would give both the same answer, which is exactly the
    // thing container queries were invented to fix.
    Shell app;
    app.ui.document.SetProp(app.sidebar, BreakpointKey("compact", "axis"), std::string("column"));
    app.ui.document.SetProp(app.body,    BreakpointKey("compact", "axis"), std::string("column"));
    app.ui.document.Touch(app.sidebar);
    app.ui.document.Touch(app.body);
    app.ui.Frame();

    CHECK(app.StyleOf(app.sidebar).axis == layout::Axis::Column);   // 280 wide
    CHECK(app.StyleOf(app.body).axis    == layout::Axis::Row);      // the rest of 1400
    CHECK_EQ(app.ui.host.Tree().NarrowestBreakpoint(app.ui.ViewOfPlain(app.sidebar)),
             std::string_view("compact"));
    CHECK(app.ui.host.Tree().NarrowestBreakpoint(app.ui.ViewOfPlain(app.body)).empty());
}

TEST(breakpoints, a_state_overlay_still_beats_one) {
    Shell app;
    // Fill transitions, and this test is about which layer wins rather than how long it takes to
    // get there — so read the settled answer instead of a frame partway into a fade.
    app.ui.host.SetMotion({ false });
    app.ui.document.SetProp(app.body, doc::Prop::Fill, Color{ 1.0f, 0.0f, 0.0f, 1.0f });
    app.ui.document.SetProp(app.body, BreakpointKey("compact", doc::Prop::Fill),
                            Color{ 0.0f, 1.0f, 0.0f, 1.0f });
    app.ui.document.SetProp(app.body, StateKey(StateBit::Hovered, doc::Prop::Fill),
                            Color{ 0.0f, 0.0f, 1.0f, 1.0f });
    app.ui.document.Touch(app.body);
    app.Width(500.0f);

    const u32 view = app.ui.ViewOfPlain(app.body);
    const doc::Value narrow = app.ui.host.Tree().ResolvedProp(view, doc::Prop::Fill);
    CHECK(std::get<Color>(narrow).g == 1.0f);

    app.ui.host.Tree().SetState(view, StateBit::Hovered, true);
    const doc::Value hovered = app.ui.host.Tree().ResolvedProp(view, doc::Prop::Fill);
    CHECK(std::get<Color>(hovered).b == 1.0f);
}

TEST(breakpoints, an_overlay_may_not_move_the_width_its_own_query_was_answered_by) {
    // The one rule that keeps a container query from arguing with itself. Height is fair game — a
    // column that grows taller is what a narrow layout does — but width is what was measured.
    Shell app;
    app.ui.document.SetProp(app.sidebar, BreakpointKey("compact", "width"), std::string("fill"));
    app.ui.document.SetProp(app.sidebar, BreakpointKey("compact", "height"), std::string("120"));
    app.ui.document.Touch(app.sidebar);
    app.ui.Frame();

    CHECK(app.StyleOf(app.sidebar).width == layout::Size::Px(280.0f));
    CHECK(app.StyleOf(app.sidebar).height == layout::Size::Px(120.0f));
}

TEST(breakpoints, a_design_that_uses_none_pays_for_none) {
    Shell app;
    CHECK(!app.ui.host.Tree().HasBreakpointOverlays());
    app.ui.document.SetProp(app.shell, BreakpointKey("compact", "axis"), std::string("column"));
    app.ui.document.Touch(app.shell);
    app.ui.Frame();
    CHECK(app.ui.host.Tree().HasBreakpointOverlays());
}

TEST(breakpoints, a_project_names_its_own_and_the_defaults_are_not_a_law) {
    Shell app;
    app.ui.document.SetBreakpoints({ { "phone", 480.0f }, { "tablet", 900.0f } });
    app.ui.document.SetProp(app.shell, BreakpointKey("phone", "axis"), std::string("column"));
    app.ui.document.Touch(app.shell);

    app.Width(700.0f);                                        // tablet, not phone
    CHECK(app.StyleOf(app.shell).axis == layout::Axis::Row);
    app.Width(400.0f);
    CHECK(app.StyleOf(app.shell).axis == layout::Axis::Column);

    // And "compact" is not a breakpoint any more, so an overlay named after it does nothing.
    app.ui.document.SetProp(app.shell, BreakpointKey("compact", "gap"), std::string("2"));
    app.ui.document.Touch(app.shell);
    app.ui.Frame();
    CHECK_EQ(app.StyleOf(app.shell).gap, 24.0f);
}

// ---------------------------------------------------------------------- inheritance

TEST(inheritance, a_font_set_on_a_container_reaches_the_labels_inside_it) {
    // The whole point: change the app's typeface in one place instead of on every text node.
    Ui ui;
    const Uuid card = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Card");
    ui.document.SetProp(card, doc::Prop::FontFamily, std::string("Inter"));
    ui.document.SetProp(card, doc::Prop::FontSize, 22.0f);
    ui.document.SetProp(card, doc::Prop::TextColor, doc::TokenRef{ "accent" });

    const Uuid inner = ui.document.CreateNode(doc::NodeKind::Frame, card, "Inner");
    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, inner, "Label");
    ui.document.SetProp(label, doc::Prop::Text, std::string("Hello"));
    ui.Frame();

    const u32 view = ui.ViewOfPlain(label);
    CHECK(view != ViewTree::kInvalid);
    // Two levels down, through a frame that says nothing about type at all.
    CHECK_EQ(ui.host.Tree().Str(view, doc::Prop::FontFamily), std::string("Inter"));
    CHECK_EQ(ui.host.Tree().Number(view, doc::Prop::FontSize, 0.0f), 22.0f);
    // A token still resolves — inheriting hands down the reference, not a colour.
    const doc::Value colour = ui.host.Tree().ResolvedProp(view, doc::Prop::TextColor);
    CHECK(doc::TypeOf(colour) == doc::ValueType::Colour);
}

TEST(inheritance, a_node_that_names_its_own_value_keeps_it) {
    Ui ui;
    const Uuid card = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Card");
    ui.document.SetProp(card, doc::Prop::FontSize, 22.0f);

    const Uuid inherits = ui.document.CreateNode(doc::NodeKind::Text, card, "Inherits");
    const Uuid decides  = ui.document.CreateNode(doc::NodeKind::Text, card, "Decides");
    ui.document.SetProp(inherits, doc::Prop::Text, std::string("big"));
    ui.document.SetProp(decides,  doc::Prop::Text, std::string("small"));
    ui.document.SetProp(decides,  doc::Prop::FontSize, 11.0f);
    ui.Frame();

    CHECK_EQ(ui.host.Tree().Number(ui.ViewOfPlain(inherits), doc::Prop::FontSize, 0.0f), 22.0f);
    CHECK_EQ(ui.host.Tree().Number(ui.ViewOfPlain(decides),  doc::Prop::FontSize, 0.0f), 11.0f);
}

TEST(inheritance, the_nearest_answer_wins_and_nothing_boxlike_is_inherited) {
    Ui ui;
    const Uuid outer = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Outer");
    ui.document.SetProp(outer, doc::Prop::FontSize, 30.0f);
    // Not inheritable: a card inside a card is not the same drawing twice.
    ui.document.SetProp(outer, doc::Prop::Fill, doc::TokenRef{ "surface" });
    ui.document.SetProp(outer, doc::Prop::CornerRadius, 12.0f);

    const Uuid middle = ui.document.CreateNode(doc::NodeKind::Frame, outer, "Middle");
    ui.document.SetProp(middle, doc::Prop::FontSize, 15.0f);
    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, middle, "Label");
    ui.document.SetProp(label, doc::Prop::Text, std::string("x"));
    ui.Frame();

    const u32 view = ui.ViewOfPlain(label);
    CHECK_EQ(ui.host.Tree().Number(view, doc::Prop::FontSize, 0.0f), 15.0f);
    CHECK_EQ(ui.host.Tree().Number(view, doc::Prop::CornerRadius, -1.0f), -1.0f);
    CHECK(!doc::IsSet(ui.host.Tree().ResolvedProp(view, doc::Prop::Fill)));
}

TEST(inheritance, an_inherited_size_is_what_the_text_is_actually_measured_at) {
    // The read being right is not the same as the drawing being right: layout has to see it too,
    // or a label inherits a font it is not laid out in.
    Ui ui;
    const Uuid card = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Card");
    doc::Node* frame = ui.document.Find(card);
    frame->layout.width = layout::Size::Hug();
    frame->layout.height = layout::Size::Hug();

    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, card, "Label");
    ui.document.SetProp(label, doc::Prop::Text, std::string("measured"));
    ui.Frame();
    const Rect small = ui.BoundsOfPlain(label);

    ui.document.SetProp(card, doc::Prop::FontSize, 48.0f);
    ui.Frame();
    const Rect large = ui.BoundsOfPlain(label);

    CHECK(large.size.x > small.size.x * 1.5f);
    CHECK(large.size.y > small.size.y * 1.5f);
}

TEST(inheritance, a_state_overlay_on_the_node_beats_what_it_inherited) {
    Ui ui;
    const Uuid card = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Card");
    ui.document.SetProp(card, doc::Prop::FontSize, 20.0f);
    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, card, "Label");
    ui.document.SetProp(label, doc::Prop::Text, std::string("x"));
    ui.document.SetProp(label, StateKey(StateBit::Hovered, doc::Prop::FontSize), 40.0f);
    ui.Frame();

    const u32 view = ui.ViewOfPlain(label);
    CHECK_EQ(ui.host.Tree().Number(view, doc::Prop::FontSize, 0.0f), 20.0f);
    ui.host.Tree().SetState(view, StateBit::Hovered, true);
    CHECK_EQ(ui.host.Tree().Number(view, doc::Prop::FontSize, 0.0f), 40.0f);
}

TEST(inheritance, it_reaches_through_an_instance_into_the_component) {
    // An instance is expanded before any of this runs, so the chain a value travels is the one on
    // screen — not the one in the file.
    Ui ui;
    const Uuid card = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Card");
    ui.document.SetProp(card, doc::Prop::FontFamily, std::string("Inter"));
    const Uuid button = ui.document.CreateInstance(ui.library.Find("Button"), card);
    ui.Frame();

    const u32 root = ui.host.Tree().ViewOf(WidgetId{ ui.ComponentRoot(button), button });
    CHECK(root != ViewTree::kInvalid);
    const u32 label = ui.host.Tree().FindRole(root, Role::None);
    // The label inside the stock Button names no family of its own, so it takes the card's.
    for (u32 i = 0; i < ui.host.Tree().ViewCount(); ++i) {
        if (ui.host.Tree().At(i).kind != doc::NodeKind::Text) continue;
        CHECK_EQ(ui.host.Tree().Str(i, doc::Prop::FontFamily), std::string("Inter"));
        break;
    }
    (void)label;
}

TEST(ui, a_stronger_state_overrides_a_weaker_one) {
    doc::PropBag source;
    source.Set(doc::Prop::Fill, Color{ 1, 0, 0, 1 });
    source.Set(StateKey(StateBit::Hovered, doc::Prop::Fill), Color{ 0, 1, 0, 1 });
    source.Set(StateKey(StateBit::Pressed, doc::Prop::Fill), Color{ 0, 0, 1, 1 });

    doc::PropBag hovered = source;
    ApplyStateOverlay(hovered, source, static_cast<StateMask>(StateBit::Hovered));
    CHECK(hovered.Colour(doc::Prop::Fill).g == 1.0f);

    // Hovered AND pressed reads as pressed, never as a blend or as whichever was applied last.
    doc::PropBag both = source;
    ApplyStateOverlay(both, source, StateBit::Hovered | StateBit::Pressed);
    CHECK(both.Colour(doc::Prop::Fill).b == 1.0f);
}

TEST(ui, disabled_beats_every_other_state) {
    doc::PropBag source;
    source.Set(StateKey(StateBit::Pressed, doc::Prop::Fill), Color{ 0, 0, 1, 1 });
    source.Set(StateKey(StateBit::Disabled, doc::Prop::Fill), Color{ 0.5f, 0.5f, 0.5f, 1 });

    doc::PropBag bag;
    ApplyStateOverlay(bag, source, StateBit::Pressed | StateBit::Disabled);
    CHECK_NEAR(bag.Colour(doc::Prop::Fill).r, 0.5f);
}

TEST(ui, roles_round_trip_through_their_names) {
    for (u16 i = 0; i < static_cast<u16>(Role::Count); ++i) {
        const auto role = static_cast<Role>(i);
        CHECK_EQ(RoleFromName(RoleName(role)).value_or(Role::Count), role);
    }
    CHECK(!RoleFromName("nonsense").has_value());
}

// ------------------------------------------------------------------ view tree

TEST(ui, an_instance_keeps_its_components_shape_and_its_own_position) {
    Ui ui;
    const Uuid a = ui.Place("Button", { 40.0f, 40.0f });
    const Uuid b = ui.Place("Button", { 300.0f, 200.0f });

    // Height comes from the component, position from the instance.
    CHECK_NEAR(ui.Bounds(a).size.y, 32.0f);
    CHECK_NEAR(ui.Bounds(b).size.y, 32.0f);
    CHECK_NEAR(ui.Bounds(a).pos.x, 40.0f);
    CHECK_NEAR(ui.Bounds(b).pos.x, 300.0f);
    CHECK_NEAR(ui.Bounds(b).pos.y, 200.0f);
}

TEST(ui, hit_testing_returns_the_topmost_view) {
    Ui ui;
    const Uuid button = ui.Place("Button", { 40.0f, 40.0f });
    const u32 hit = ui.host.Tree().HitTest(ui.Center(button));
    CHECK(hit != ViewTree::kInvalid);
    // The label sits on top of the button; the behavior still belongs to the button.
    CHECK_EQ(ui.host.Tree().BehaviorOwner(hit), ui.View(button));

    // Empty screen: the hit is the screen itself, and nothing there answers to a behavior.
    const u32 background = ui.host.Tree().HitTest({ 5.0f, 5.0f });
    CHECK_EQ(background, ui.host.Tree().Root());
    CHECK_EQ(ui.host.Tree().BehaviorOwner(background), ViewTree::kInvalid);
    // Outside the screen entirely is a miss.
    CHECK_EQ(ui.host.Tree().HitTest({ -5.0f, -5.0f }), ViewTree::kInvalid);
}

TEST(ui, a_hidden_node_takes_no_space) {
    Ui ui;
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Row");
    doc::Node* node = ui.document.Find(row);
    node->layout.mode = layout::LayoutMode::Stack;
    node->layout.axis = layout::Axis::Row;
    node->layout.gap = 10.0f;

    for (int i = 0; i < 3; ++i) {
        const Uuid child = ui.document.CreateNode(doc::NodeKind::Frame, row, "Child");
        doc::Node* c = ui.document.Find(child);
        c->layout.width = layout::Size::Px(50.0f);
        c->layout.height = layout::Size::Px(20.0f);
        if (i == 1) c->visible = false;
    }
    ui.Frame();

    // Two visible children plus one gap, not three children, two gaps and a hole.
    const u32 view = ui.host.Tree().ViewOf(WidgetId{ row });
    CHECK_NEAR(ui.host.Tree().Bounds(view).size.x, 110.0f);
}

// ------------------------------------------------------------------ button

TEST(ui, a_button_clicks_on_press_and_release_inside) {
    Ui ui;
    const Uuid button = ui.Place("Button");

    ui.Move(ui.Center(button));
    CHECK(HasState(ui.State(button), StateBit::Hovered));

    ui.Press(ui.Center(button));
    CHECK(HasState(ui.State(button), StateBit::Pressed));
    CHECK(!ui.Fired(ActionKind::Clicked));

    ui.Release(ui.Center(button));
    CHECK(ui.Fired(ActionKind::Clicked));
    CHECK(!HasState(ui.State(button), StateBit::Pressed));
}

TEST(ui, a_click_survives_the_document_changing_between_press_and_release) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, ui.screen, "Elsewhere");

    ui.Press(ui.Center(button));
    CHECK(HasState(ui.State(button), StateBit::Pressed));

    // Something else on the screen changes, which rebuilds the view tree from scratch. A press is
    // held by the host and keyed by id, so it has to survive that — anything that writes the
    // document every frame (an animation, a debugger holding a value) would otherwise make the
    // whole app unclickable, and the symptom would look nothing like the cause.
    ui.document.SetProp(label, doc::Prop::Text, std::string("changed"));
    ui.Frame();
    CHECK(HasState(ui.State(button), StateBit::Pressed));

    ui.Release(ui.Center(button));
    CHECK(ui.Fired(ActionKind::Clicked));
}

TEST(ui, writing_a_property_its_value_already_is_does_not_disturb_the_document) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    const Uuid root = ui.ComponentRoot(button);
    ui.document.SetOverride(button, root, doc::Prop::CornerRadius, 9.0f);

    const u64 revision = ui.document.Revision();
    ui.document.SetOverride(button, root, doc::Prop::CornerRadius, 9.0f);
    ui.document.SetProp(ui.screen, doc::Prop::ClipContent,
                        ui.document.GetProp(ui.screen, doc::Prop::ClipContent));
    CHECK(ui.document.Revision() == revision);

    ui.document.SetOverride(button, root, doc::Prop::CornerRadius, 10.0f);
    CHECK(ui.document.Revision() != revision);
}

TEST(ui, releasing_outside_a_button_cancels_the_click) {
    Ui ui;
    const Uuid button = ui.Place("Button");

    ui.Press(ui.Center(button));
    ui.Move({ 600.0f, 500.0f });
    CHECK(!HasState(ui.State(button), StateBit::Pressed));
    ui.Release({ 600.0f, 500.0f });
    CHECK(!ui.Fired(ActionKind::Clicked));
}

TEST(ui, a_disabled_button_neither_clicks_nor_leaks_the_click) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    ui.document.SetOverride(button, ui.ComponentRoot(button), doc::Prop::Enabled, false);
    ui.Settle();

    CHECK(HasState(ui.State(button), StateBit::Disabled));
    const bool handled = ui.host.Dispatch(
        MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                        ui.Center(button).x, ui.Center(button).y, Mod::None));
    CHECK(handled);                          // swallowed, not passed through to the screen
    ui.Release(ui.Center(button));
    CHECK(!ui.Fired(ActionKind::Clicked));
}

TEST(ui, space_activates_the_focused_button) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    ui.ClickOn(button);
    ui.Drain();
    CHECK_EQ(ui.host.Focused(), ui.View(button));

    ui.Key(Key::Space);
    CHECK(ui.Fired(ActionKind::Clicked));
}

TEST(ui, two_instances_of_one_button_are_independent) {
    Ui ui;
    const Uuid a = ui.Place("Button", { 40.0f, 40.0f });
    const Uuid b = ui.Place("Button", { 40.0f, 200.0f });

    ui.Move(ui.Center(a));
    CHECK(HasState(ui.State(a), StateBit::Hovered));
    CHECK(!HasState(ui.State(b), StateBit::Hovered));
}

// ------------------------------------------------------------------ toggles

TEST(ui, a_checkbox_toggles_and_reports_its_new_value) {
    Ui ui;
    const Uuid box = ui.Place("Checkbox");
    CHECK(!ui.Flag(box, doc::Prop::Checked));

    ui.ClickOn(box);
    ui.Frame();
    CHECK(ui.Flag(box, doc::Prop::Checked));
    CHECK(HasState(ui.State(box), StateBit::Checked));

    const auto actions = ui.Drain();
    CHECK_EQ(actions.size(), std::size_t(1));
    CHECK_EQ(actions[0].kind, ActionKind::ValueChanged);
    CHECK_EQ(std::get<bool>(actions[0].value), true);

    ui.ClickOn(box);
    ui.Frame();
    CHECK(!ui.Flag(box, doc::Prop::Checked));
}

TEST(ui, checking_a_checkbox_shows_its_tick) {
    Ui ui;
    const Uuid box = ui.Place("Checkbox");
    const u32 tick = ui.host.Tree().FindRole(ui.View(box), Role::Indicator);
    CHECK(tick != ViewTree::kInvalid);
    CHECK(!ui.host.Tree().At(tick).visible);

    ui.ClickOn(box);
    ui.Frame();
    CHECK(ui.host.Tree().At(ui.host.Tree().FindRole(ui.View(box), Role::Indicator)).visible);
}

TEST(ui, only_one_radio_in_a_group_stays_checked) {
    Ui ui;
    const Uuid a = ui.Place("Radio", { 40.0f, 40.0f });
    const Uuid b = ui.Place("Radio", { 40.0f, 100.0f });
    const Uuid other = ui.Place("Radio", { 40.0f, 160.0f });
    for (Uuid id : { a, b })
        ui.document.SetOverride(id, ui.ComponentRoot(id), doc::Prop::Group, std::string("size"));
    ui.document.SetOverride(other, ui.ComponentRoot(other), doc::Prop::Group, std::string("colour"));
    ui.Frame();

    ui.ClickOn(a);
    ui.Frame();
    ui.ClickOn(other);
    ui.Frame();
    CHECK(ui.Flag(a, doc::Prop::Checked));
    CHECK(ui.Flag(other, doc::Prop::Checked));

    ui.ClickOn(b);
    ui.Frame();
    CHECK(ui.Flag(b, doc::Prop::Checked));
    CHECK(!ui.Flag(a, doc::Prop::Checked));
    // A different group is a different question, and answering one must not clear the other.
    CHECK(ui.Flag(other, doc::Prop::Checked));
}

TEST(ui, clicking_the_selected_radio_keeps_it_selected) {
    Ui ui;
    const Uuid radio = ui.Place("Radio");
    ui.ClickOn(radio);
    ui.Frame();
    ui.Drain();

    ui.ClickOn(radio);
    ui.Frame();
    CHECK(ui.Flag(radio, doc::Prop::Checked));
    CHECK(!ui.Fired(ActionKind::ValueChanged));
}

TEST(ui, a_switch_slides_its_knob_across_the_track) {
    Ui ui;
    const Uuid toggle = ui.Place("Switch");
    const u32 knob = ui.host.Tree().FindRole(ui.View(toggle), Role::Knob);
    const f32 off = ui.host.Tree().Bounds(knob).pos.x;

    ui.ClickOn(toggle);
    ui.Frame();
    const f32 on = ui.host.Tree().Bounds(ui.host.Tree().FindRole(ui.View(toggle), Role::Knob)).pos.x;
    CHECK(on > off + 10.0f);
}

// ------------------------------------------------------------------ slider

TEST(ui, dragging_a_slider_tracks_the_pointer) {
    Ui ui;
    const Uuid slider = ui.Place("Slider");
    const Rect bounds = ui.Bounds(slider);

    ui.Press({ bounds.Left(), bounds.Center().y });
    ui.Frame();
    CHECK_NEAR(ui.Number(slider, doc::Prop::Value), 0.0f);

    ui.Move({ bounds.Right(), bounds.Center().y });
    ui.Frame();
    CHECK_NEAR(ui.Number(slider, doc::Prop::Value), 1.0f);

    // Past either end it saturates rather than wrapping or running away.
    ui.Move({ bounds.Right() + 500.0f, bounds.Center().y });
    ui.Frame();
    CHECK_NEAR(ui.Number(slider, doc::Prop::Value), 1.0f);
    ui.Release({ bounds.Right(), bounds.Center().y });
}

TEST(ui, a_slider_quantizes_to_its_step) {
    Ui ui;
    const Uuid slider = ui.Place("Slider");
    ui.document.SetOverride(slider, ui.ComponentRoot(slider), doc::Prop::MaxValue, 10.0f);
    ui.document.SetOverride(slider, ui.ComponentRoot(slider), doc::Prop::Step, 2.0f);
    ui.document.SetOverride(slider, ui.ComponentRoot(slider), doc::Prop::Value, 0.0f);
    ui.Frame();

    const Rect bounds = ui.Bounds(slider);
    ui.Press({ bounds.Left() + bounds.size.x * 0.47f, bounds.Center().y });
    ui.Frame();
    const f32 value = ui.Number(slider, doc::Prop::Value);
    CHECK_NEAR(std::fmod(value, 2.0f), 0.0f);
    ui.Release({ bounds.Center().x, bounds.Center().y });
}

TEST(ui, arrow_keys_step_a_focused_slider) {
    Ui ui;
    const Uuid slider = ui.Place("Slider");
    ui.document.SetOverride(slider, ui.ComponentRoot(slider), doc::Prop::Step, 0.1f);
    ui.document.SetOverride(slider, ui.ComponentRoot(slider), doc::Prop::Value, 0.5f);
    ui.Frame();

    ui.Press(ui.Center(slider));
    ui.Release(ui.Center(slider));
    ui.Frame();
    ui.document.SetOverride(slider, ui.ComponentRoot(slider), doc::Prop::Value, 0.5f);
    ui.Frame();

    ui.Key(Key::Right);
    ui.Frame();
    CHECK_NEAR(ui.Number(slider, doc::Prop::Value), 0.6f);
    ui.Key(Key::Home);
    ui.Frame();
    CHECK_NEAR(ui.Number(slider, doc::Prop::Value), 0.0f);
    ui.Key(Key::End);
    ui.Frame();
    CHECK_NEAR(ui.Number(slider, doc::Prop::Value), 1.0f);
}

TEST(ui, a_slider_moves_the_filled_part_of_its_track) {
    Ui ui;
    const Uuid slider = ui.Place("Slider");
    const u32 fill = ui.host.Tree().FindRole(ui.View(slider), Role::Fill);
    CHECK(fill != ViewTree::kInvalid);
    const f32 half = ui.host.Tree().Bounds(fill).size.x;

    const Rect bounds = ui.Bounds(slider);
    ui.Press({ bounds.Right() - 1.0f, bounds.Center().y });
    ui.Release({ bounds.Right() - 1.0f, bounds.Center().y });
    ui.Frame();
    CHECK(ui.host.Tree().Bounds(ui.host.Tree().FindRole(ui.View(slider), Role::Fill)).size.x
          > half + 10.0f);
}

// ------------------------------------------------------------------ text input

TEST(ui, typing_lands_in_a_focused_field) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("hello");
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("hello"));
    CHECK_EQ(ui.CountOf(ActionKind::TextChanged), 5u);
}

TEST(ui, backspace_and_delete_remove_one_codepoint_not_one_byte) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("naïve");
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("naïve"));

    for (int i = 0; i < 3; ++i) ui.Key(Key::Backspace);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("na"));

    ui.Key(Key::Home);
    ui.Key(Key::Delete);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("a"));
}

TEST(ui, ctrl_a_selects_all_and_typing_replaces_it) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("replace me");

    ui.Key(Key::A, Mod::Control);
    ui.Type("x");
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("x"));
}

TEST(ui, copy_cut_and_paste_go_through_the_clipboard) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("abcdef");

    ui.Key(Key::A, Mod::Control);
    ui.Key(Key::C, Mod::Control);
    CHECK_EQ(ui.host.GetClipboard().GetText(), std::string("abcdef"));

    ui.Key(Key::End);
    ui.Key(Key::V, Mod::Control);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("abcdefabcdef"));

    ui.Key(Key::A, Mod::Control);
    ui.Key(Key::X, Mod::Control);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string());
    CHECK_EQ(ui.host.GetClipboard().GetText(), std::string("abcdefabcdef"));
}

// The mechanism a screen reader's typing rides on: put the selection where the client asked for
// it, then deliver the text as keystrokes. Nothing reaches into the string, because the field's
// own rules — its maximum length, its refusal of newlines, the label it keeps in step — are
// exactly the rules an edit made through AT-SPI has to obey as well.
TEST(ui, an_edit_placed_by_offset_replaces_that_range_and_nothing_else) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("naïve text");

    const WidgetId id{ ui.ComponentRoot(field), field };
    TextEditState& edit = ui.host.EditState(id);
    // Bytes, because that is what an edit state counts — characters 2 to 6, which is not the
    // same pair of numbers in a string with an ï in it: "ïve " is four characters and five bytes.
    edit.anchor = 2;
    edit.caret  = 7;
    ui.Type("ï");
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("naïtext"));

    // And an empty insertion is a deletion, which is the same key the user would press.
    edit.anchor = 0;
    edit.caret  = 4;
    ui.Key(Key::Backspace);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("text"));
}

TEST(ui, an_edit_still_obeys_the_fields_own_rules) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::MaxLength, 6.0f);
    ui.Frame();
    ui.ClickOn(field);
    ui.Type("abcdef");

    const WidgetId id{ ui.ComponentRoot(field), field };
    TextEditState& edit = ui.host.EditState(id);
    edit.anchor = edit.caret = 6;
    ui.Type("ghi");
    // Not seven characters, because the field says six — a screen reader typing past the end of
    // a field is stopped by the same rule a keyboard is.
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("abcdef"));
}

TEST(ui, a_read_only_field_takes_no_typing_at_all) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::Text,
                            std::string("fixed"));
    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::ReadOnly, true);
    ui.Frame();
    ui.ClickOn(field);
    ui.Type("x");
    ui.Key(Key::Backspace);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("fixed"));
}

TEST(ui, ctrl_arrow_moves_by_words) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("one two three");

    ui.Key(Key::Left, Mod::Control);
    ui.Key(Key::Backspace, Mod::Control);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("one three"));
}

TEST(ui, a_single_line_field_submits_on_enter_and_a_multiline_one_does_not) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("value");
    ui.Drain();

    ui.Key(Key::Enter);
    CHECK(ui.Fired(ActionKind::Submitted));
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("value"));

    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::Multiline, true);
    ui.Frame();
    ui.Drain();
    ui.Key(Key::Enter);
    CHECK(!ui.Fired(ActionKind::Submitted));
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("value\n"));
}

TEST(ui, max_length_truncates_a_paste_instead_of_rejecting_it) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::MaxLength, 5.0f);
    ui.Frame();

    ui.ClickOn(field);
    ui.host.GetClipboard().SetText("abcdefghij");
    ui.Key(Key::V, Mod::Control);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("abcde"));
}

TEST(ui, a_read_only_field_can_be_copied_but_not_changed) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::Text, std::string("locked"));
    ui.document.SetOverride(field, ui.ComponentRoot(field), doc::Prop::ReadOnly, true);
    ui.Frame();

    ui.ClickOn(field);
    ui.Type("nope");
    ui.Key(Key::Backspace);
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("locked"));

    ui.Key(Key::A, Mod::Control);
    ui.Key(Key::C, Mod::Control);
    CHECK_EQ(ui.host.GetClipboard().GetText(), std::string("locked"));
}

TEST(ui, a_caret_survives_the_document_being_rebuilt) {
    Ui ui;
    const Uuid field = ui.Place("TextInput");
    ui.ClickOn(field);
    ui.Type("abc");
    ui.Key(Key::Home);

    // Any unrelated edit rebuilds the whole view tree; the caret must not jump.
    ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Unrelated");
    ui.Frame();

    ui.Type("Z");
    CHECK_EQ(ui.Str(field, doc::Prop::Text), std::string("Zabc"));
}

TEST(ui, two_text_fields_keep_separate_carets) {
    Ui ui;
    const Uuid a = ui.Place("TextInput", { 40.0f, 40.0f });
    const Uuid b = ui.Place("TextInput", { 40.0f, 120.0f });

    ui.ClickOn(a);
    ui.Type("aaa");
    ui.ClickOn(b);
    ui.Type("bbb");
    CHECK_EQ(ui.Str(a, doc::Prop::Text), std::string("aaa"));
    CHECK_EQ(ui.Str(b, doc::Prop::Text), std::string("bbb"));
}

// ------------------------------------------------------------------ dropdown

TEST(ui, a_dropdown_opens_a_menu_and_a_choice_closes_it) {
    Ui ui;
    const Uuid dropdown = ui.Place("Dropdown");
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));

    ui.ClickOn(dropdown);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));
    CHECK(HasState(ui.State(dropdown), StateBit::Open));

    const ViewTree& menu = *ui.host.OverlayAt(0).tree;
    const auto items = menu.FindAllRoles(menu.Root(), Role::DropdownItem);
    CHECK_EQ(items.size(), std::size_t(3));

    ui.Click(menu.Bounds(items[1]).Center());
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
    CHECK_NEAR(ui.Number(dropdown, doc::Prop::SelectedIndex), 1.0f);
    CHECK(ui.Fired(ActionKind::SelectionChanged));
}

TEST(ui, clicking_outside_an_open_menu_dismisses_it_without_pressing_what_is_underneath) {
    Ui ui;
    const Uuid dropdown = ui.Place("Dropdown", { 40.0f, 40.0f });
    const Uuid button = ui.Place("Button", { 400.0f, 400.0f });

    ui.ClickOn(dropdown);
    ui.Frame();
    ui.Drain();

    ui.Click(ui.Center(button));
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
    CHECK(!ui.Fired(ActionKind::Clicked));
}

TEST(ui, choosing_from_one_dropdown_leaves_the_other_alone) {
    Ui ui;
    const Uuid a = ui.Place("Dropdown", { 40.0f, 40.0f });
    const Uuid b = ui.Place("Dropdown", { 40.0f, 200.0f });

    ui.ClickOn(b);
    ui.Frame();
    const ViewTree& menu = *ui.host.OverlayAt(0).tree;
    const auto items = menu.FindAllRoles(menu.Root(), Role::DropdownItem);
    ui.Click(menu.Bounds(items[2]).Center());
    ui.Frame();

    CHECK_NEAR(ui.Number(b, doc::Prop::SelectedIndex), 2.0f);
    CHECK_NEAR(ui.Number(a, doc::Prop::SelectedIndex), -1.0f);
}

// ------------------------------------------------------------------ tabs

TEST(ui, selecting_a_tab_switches_the_visible_panel) {
    Ui ui;
    const Uuid tabs = ui.Place("Tabs");
    const ViewTree& tree = ui.host.Tree();
    const auto strip = tree.FindAllRoles(ui.View(tabs), Role::Tab);
    const auto panels = tree.FindAllRoles(ui.View(tabs), Role::Content);
    CHECK_EQ(strip.size(), std::size_t(3));
    CHECK(tree.At(panels[0]).visible);
    CHECK(!tree.At(panels[1]).visible);

    ui.Click(tree.Bounds(strip[1]).Center());
    ui.Frame();
    CHECK_NEAR(ui.Number(tabs, doc::Prop::SelectedIndex), 1.0f);

    const auto after = ui.host.Tree().FindAllRoles(ui.View(tabs), Role::Content);
    CHECK(!ui.host.Tree().At(after[0]).visible);
    CHECK(ui.host.Tree().At(after[1]).visible);
    CHECK(ui.Fired(ActionKind::SelectionChanged));
}

TEST(ui, arrow_keys_wrap_around_the_tab_strip) {
    Ui ui;
    const Uuid tabs = ui.Place("Tabs");
    const ViewTree& tree = ui.host.Tree();
    ui.Click(tree.Bounds(tree.FindAllRoles(ui.View(tabs), Role::Tab)[0]).Center());
    ui.Frame();
    ui.host.Focus(ui.View(tabs));

    ui.Key(Key::Left);
    ui.Frame();
    CHECK_NEAR(ui.Number(tabs, doc::Prop::SelectedIndex), 2.0f);
    ui.Key(Key::Right);
    ui.Frame();
    CHECK_NEAR(ui.Number(tabs, doc::Prop::SelectedIndex), 0.0f);
}

// ------------------------------------------------------------------ scrolling

TEST(ui, a_scroll_view_scrolls_and_clamps_at_both_ends) {
    Ui ui;
    const Uuid scroll = ui.Place("Scroll");
    const u32 content = ui.host.Tree().FindRole(ui.View(scroll), Role::Content);
    for (int i = 0; i < 20; ++i) {
        const Uuid row = ui.document.CreateNode(
            doc::NodeKind::Frame, ui.host.Tree().At(content).sourceId, "Row");
        ui.document.Find(row)->layout.height = layout::Size::Px(30.0f);
    }
    ui.Frame();

    ui.Move(ui.Center(scroll));
    ui.Wheel(-3.0f);
    ui.Frame();
    const f32 scrolled = ui.host.Tree().At(ui.View(scroll)).scroll.y;
    CHECK(scrolled > 0.0f);

    for (int i = 0; i < 50; ++i) ui.Wheel(-3.0f);
    ui.Frame();
    const f32 limit = ui.host.Tree().At(ui.View(scroll)).scroll.y;
    CHECK(limit > scrolled);

    for (int i = 0; i < 100; ++i) ui.Wheel(3.0f);
    ui.Frame();
    CHECK_NEAR(ui.host.Tree().At(ui.View(scroll)).scroll.y, 0.0f);
}

TEST(ui, an_empty_scroll_view_does_not_scroll) {
    Ui ui;
    const Uuid scroll = ui.Place("Scroll");
    ui.Move(ui.Center(scroll));
    ui.Wheel(-5.0f);
    ui.Frame();
    CHECK_NEAR(ui.host.Tree().At(ui.View(scroll)).scroll.y, 0.0f);
}

// ------------------------------------------------------------------ virtualized list

namespace {
    struct Rows final : UiHost::ListDataSource {
        explicit Rows(u32 count) : m_Count(count) {}
        u32 Count() const override { return m_Count; }
        std::string Cell(u32 row, u32 column) const override {
            ++m_Reads;
            return "r" + std::to_string(row) + "c" + std::to_string(column);
        }
        u32 m_Count;
        mutable u32 m_Reads = 0;
    };
}

TEST(ui, a_list_selects_by_click_and_by_keyboard) {
    Ui ui;
    const Uuid list = ui.Place("List");
    ui.host.SetDataSource({ ui.ComponentRoot(list), list }, CreateRef<Rows>(1000u));
    ui.Frame();

    const Rect bounds = ui.Bounds(list);
    ui.Click({ bounds.Center().x, bounds.Top() + 28.0f * 2.5f });
    ui.Frame();
    CHECK_NEAR(ui.Number(list, doc::Prop::SelectedIndex), 2.0f);
    CHECK(ui.Fired(ActionKind::SelectionChanged));

    ui.host.Focus(ui.View(list));
    ui.Key(Key::Down);
    ui.Frame();
    CHECK_NEAR(ui.Number(list, doc::Prop::SelectedIndex), 3.0f);
    ui.Key(Key::End);
    ui.Frame();
    CHECK_NEAR(ui.Number(list, doc::Prop::SelectedIndex), 999.0f);
    // Selecting the last of a thousand rows must have scrolled it into view.
    CHECK(ui.host.Tree().At(ui.View(list)).scroll.y > 0.0f);
}

TEST(ui, a_million_row_list_only_touches_the_rows_on_screen) {
    Ui ui;
    const Uuid list = ui.Place("List");
    auto rows = CreateRef<Rows>(1'000'000u);
    ui.host.SetDataSource({ ui.ComponentRoot(list), list }, rows);
    ui.Frame();

    draw::DrawList drawList;
    PaintContext paint;
    paint.list = &drawList;
    ui.host.Paint(paint);

    // 240px of list at 28px a row is nine rows plus overscan — not a million.
    CHECK(rows->m_Reads == 0);   // no atlas, so no text was read at all
    CHECK(!drawList.Batches().empty());

    const u32 quads = static_cast<u32>(drawList.Quads().size());
    CHECK(quads < 200);
}

TEST(ui, a_list_row_template_is_never_drawn_as_itself) {
    Ui ui;
    const Uuid list = ui.Place("List");
    const u32 templateRow = ui.host.Tree().FindRole(ui.View(list), Role::ListItem);
    CHECK(templateRow != ViewTree::kInvalid);
    CHECK(!ui.host.Tree().At(templateRow).visible);
}

TEST(ui, a_table_offsets_its_rows_below_the_header) {
    Ui ui;
    const Uuid table = ui.Place("Table");
    ui.host.SetDataSource({ ui.ComponentRoot(table), table }, CreateRef<Rows>(100u));
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    CHECK_EQ(tree.FindAllRoles(ui.View(table), Role::TableColumn).size(), std::size_t(3));

    const Rect bounds = ui.Bounds(table);
    // A click on the header row is not a click on row 0.
    ui.Click({ bounds.Center().x, bounds.Top() + 10.0f });
    ui.Frame();
    CHECK_NEAR(ui.Number(table, doc::Prop::SelectedIndex), -1.0f);

    ui.Click({ bounds.Center().x, bounds.Top() + 28.0f + 10.0f });
    ui.Frame();
    CHECK_NEAR(ui.Number(table, doc::Prop::SelectedIndex), 0.0f);
}

// ------------------------------------------------------------------ overlays

TEST(ui, a_modal_blocks_what_is_underneath_and_its_scrim_dismisses_it) {
    Ui ui;
    const Uuid button = ui.Place("Button", { 40.0f, 40.0f });
    const Uuid modal = ui.document.CreateInstance(ui.library.Find("Modal"), ui.screen);
    ui.document.Find(modal)->visible = false;
    ui.Frame();

    ui.host.OpenOverlay({ ui.ComponentRoot(modal), modal }, ui.ComponentRoot(modal), true);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));

    ui.Drain();
    ui.Click(ui.Center(button));
    CHECK(!ui.Fired(ActionKind::Clicked));

    ui.Click({ 700.0f, 550.0f });     // the scrim
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
    CHECK(ui.Fired(ActionKind::Dismissed));
}

TEST(ui, escape_closes_the_top_overlay) {
    Ui ui;
    const Uuid dropdown = ui.Place("Dropdown");
    ui.ClickOn(dropdown);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));

    ui.Key(Key::Escape);
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
}

TEST(ui, a_toast_dismisses_itself_when_its_time_runs_out) {
    Ui ui;
    const Uuid toast = ui.document.CreateInstance(ui.library.Find("Toast"), ui.screen);
    ui.document.Find(toast)->visible = false;
    ui.Frame();

    ui.host.OpenOverlay({ ui.ComponentRoot(toast), toast }, ui.ComponentRoot(toast), false, {}, 1.0f);
    ui.Frame(0.5f);
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));
    ui.Frame(0.6f);
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
}

TEST(ui, a_popover_is_placed_against_its_anchor_and_kept_on_screen) {
    Ui ui;
    const Uuid popover = ui.document.CreateInstance(ui.library.Find("Popover"), ui.screen);
    ui.document.Find(popover)->visible = false;
    ui.Frame();

    const Rect anchor{ { 700.0f, 560.0f }, { 80.0f, 30.0f } };
    ui.host.OpenOverlay({ ui.ComponentRoot(popover), popover }, ui.ComponentRoot(popover),
                        false, anchor);
    ui.Frame();

    const ViewTree& tree = *ui.host.OverlayAt(0).tree;
    const Rect bounds = tree.Bounds(tree.Root());
    CHECK(bounds.Bottom() <= ui.size.y + 0.5f);
    CHECK(bounds.Right() <= ui.size.x + 0.5f);
}

// ------------------------------------------------------------------ router

TEST(ui, a_router_shows_one_screen_and_remembers_where_it_came_from) {
    Ui ui;
    const Uuid router = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Router");
    ui.document.SetProp(router, doc::Prop::Role, std::string(RoleName(Role::Router)));
    for (const char* name : { "Home", "Settings", "About" })
        ui.document.CreateNode(doc::NodeKind::Frame, router, name);
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    const u32 view = tree.ViewOf(WidgetId{ router });
    CHECK(tree.At(tree.At(view).children[0]).visible);
    CHECK(!tree.At(tree.At(view).children[1]).visible);

    ui.host.Navigate(WidgetId{ router }, "Settings");
    ui.Frame();
    const u32 after = ui.host.Tree().ViewOf(WidgetId{ router });
    CHECK(!ui.host.Tree().At(ui.host.Tree().At(after).children[0]).visible);
    CHECK(ui.host.Tree().At(ui.host.Tree().At(after).children[1]).visible);
    CHECK(ui.Fired(ActionKind::Navigated));

    ui.host.Navigate(WidgetId{ router }, "About");
    ui.Frame();
    CHECK_EQ(ui.host.HistoryDepth(WidgetId{ router }), std::size_t(2));

    CHECK(ui.host.Back(WidgetId{ router }));
    ui.Frame();
    CHECK_EQ(ui.host.Route(WidgetId{ router }), std::string("Settings"));
    CHECK(ui.host.Back(WidgetId{ router }));
    CHECK(!ui.host.Back(WidgetId{ router }));
}

TEST(ui, a_router_with_an_unknown_route_still_shows_something) {
    Ui ui;
    const Uuid router = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Router");
    ui.document.SetProp(router, doc::Prop::Role, std::string(RoleName(Role::Router)));
    ui.document.SetProp(router, doc::Prop::Route, std::string("Nowhere"));
    ui.document.CreateNode(doc::NodeKind::Frame, router, "Home");
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    const u32 view = tree.ViewOf(WidgetId{ router });
    CHECK(tree.At(tree.At(view).children[0]).visible);
}

// ------------------------------------------------------------------ focus

TEST(ui, tab_walks_the_focusable_widgets_and_shift_tab_walks_back) {
    Ui ui;
    const Uuid a = ui.Place("Button", { 40.0f, 40.0f });
    const Uuid b = ui.Place("TextInput", { 40.0f, 120.0f });
    const Uuid c = ui.Place("Checkbox", { 40.0f, 200.0f });

    ui.host.Dispatch(MakeKey(EventType::KeyPressed, Key::Tab, Mod::None));
    CHECK_EQ(ui.host.Focused(), ui.View(a));
    ui.host.Dispatch(MakeKey(EventType::KeyPressed, Key::Tab, Mod::None));
    CHECK_EQ(ui.host.Focused(), ui.View(b));
    ui.host.Dispatch(MakeKey(EventType::KeyPressed, Key::Tab, Mod::None));
    CHECK_EQ(ui.host.Focused(), ui.View(c));
    ui.host.Dispatch(MakeKey(EventType::KeyPressed, Key::Tab, Mod::Shift));
    CHECK_EQ(ui.host.Focused(), ui.View(b));
}

TEST(ui, focus_skips_a_disabled_widget) {
    Ui ui;
    const Uuid a = ui.Place("Button", { 40.0f, 40.0f });
    const Uuid b = ui.Place("Button", { 40.0f, 120.0f });
    const Uuid c = ui.Place("Button", { 40.0f, 200.0f });
    ui.document.SetOverride(b, ui.ComponentRoot(b), doc::Prop::Enabled, false);
    ui.Frame();

    ui.host.Dispatch(MakeKey(EventType::KeyPressed, Key::Tab, Mod::None));
    CHECK_EQ(ui.host.Focused(), ui.View(a));
    ui.host.Dispatch(MakeKey(EventType::KeyPressed, Key::Tab, Mod::None));
    CHECK_EQ(ui.host.Focused(), ui.View(c));
}

TEST(ui, the_cursor_reflects_what_is_under_the_pointer) {
    Ui ui;
    const Uuid button = ui.Place("Button", { 40.0f, 40.0f });
    const Uuid field = ui.Place("TextInput", { 40.0f, 200.0f });

    ui.Move(ui.Center(button));
    CHECK_EQ(ui.host.Cursor(), CursorShape::Hand);
    ui.Move(ui.Center(field));
    CHECK_EQ(ui.host.Cursor(), CursorShape::IBeam);
    ui.Move({ 700.0f, 550.0f });
    CHECK_EQ(ui.host.Cursor(), CursorShape::Arrow);

    ui.document.SetOverride(button, ui.ComponentRoot(button), doc::Prop::Enabled, false);
    ui.Frame();
    ui.Move(ui.Center(button));
    CHECK_EQ(ui.host.Cursor(), CursorShape::NotAllowed);
}

// ------------------------------------------------------------------ painting

TEST(ui, two_copies_of_a_component_do_not_share_a_widget_nested_inside_it) {
    // A catalog is components made of components: a Row holds a Checkbox, and a screen holds two
    // Rows. The checkbox node is authored once, inside Row, so without per-copy resolution ticking
    // one row's box ticks the other's — which is the bug that makes a list of settings unusable.
    Ui ui;

    const Uuid rowRoot = ui.document.CreateNode(doc::NodeKind::Frame, Uuid::Invalid(), "Row");
    {
        doc::Node* node = ui.document.Find(rowRoot);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.width = layout::Size::Px(200.0f);
        node->layout.height = layout::Size::Px(40.0f);
    }
    ui.document.CreateInstance(ui.library.Find("Checkbox"), rowRoot);
    const Uuid row = ui.document.MakeComponent(rowRoot, "Row");

    const Uuid first  = ui.document.CreateInstance(row, ui.screen);
    const Uuid second = ui.document.CreateInstance(row, ui.screen);
    ui.document.Find(first)->layout.offsetStart  = { 40.0f, 40.0f };
    ui.document.Find(second)->layout.offsetStart = { 40.0f, 140.0f };
    ui.document.Touch(first);
    ui.document.Touch(second);
    ui.Frame();

    // The checkbox inside each row: one view per copy, found by walking down from the row.
    const auto BoxIn = [&](Uuid instance) {
        const u32 rowView = ui.View(instance);
        const auto& children = ui.host.Tree().At(rowView).children;
        return children.empty() ? ViewTree::kInvalid : children.front();
    };

    const u32 boxOne = BoxIn(first);
    const u32 boxTwo = BoxIn(second);
    CHECK(boxOne != ViewTree::kInvalid);
    CHECK(boxTwo != ViewTree::kInvalid);
    CHECK(boxOne != boxTwo);

    ui.Click(ui.host.Tree().Bounds(boxOne).Center());
    ui.Frame();

    CHECK(ui.host.Tree().Flag(BoxIn(first), doc::Prop::Checked));
    CHECK(!ui.host.Tree().Flag(BoxIn(second), doc::Prop::Checked));
}

TEST(ui, the_test_harness_has_real_text_metrics) {
    // Everything below measures text. A harness with no font measures all of it as zero, which
    // turns a whole class of layout assertion into one that cannot fail.
    Ui ui;
    const Uuid button = ui.Place("Button");
    const u32 label = ui.host.Tree().FindByName("Label");
    CHECK(label != ViewTree::kInvalid);
    CHECK(ui.host.Tree().Bounds(label).size.x > 20.0f);
    CHECK(ui.host.Tree().Bounds(label).size.y > 8.0f);
    (void)button;
}

TEST(ui, every_component_in_the_catalog_places_and_paints) {
    // A catalog is only worth having if dropping any of it on a screen works. One test over the
    // whole library beats one per component and, unlike one per component, it cannot be forgotten
    // when the next one is added.
    Ui ui;
    std::vector<std::string> names;
    for (const auto& [name, id] : ui.library.components) names.push_back(name);
    CHECK(names.size() >= 53);

    for (const std::string& name : names) {
        const Uuid instance = ui.Place(name, { 40.0f, 40.0f });
        CHECK_MESSAGE(instance.Valid(), name + ": placed");
        if (!instance.Valid()) continue;

        const u32 view = ui.View(instance);
        CHECK_MESSAGE(view != ViewTree::kInvalid, name + ": has a view");
        if (view == ViewTree::kInvalid) continue;

        const Rect box = ui.host.Tree().Bounds(view);
        CHECK_MESSAGE(box.size.x > 0.0f && box.size.y > 0.0f,
                      name + ": has a size (" + std::to_string(box.size.x) + " x "
                          + std::to_string(box.size.y) + ")");
        // Nothing may spill above or left of where it was put: a component whose own layout puts a
        // part outside it is one that will overlap whatever it is next to.
        CHECK_MESSAGE(box.pos.x >= 39.0f && box.pos.y >= 39.0f, name + ": starts where it was put");

        ui.document.DeleteNode(instance);
        ui.Frame();
    }

    // Every field in the catalog shows its text on a node a field will actually find. A widget
    // whose first text node is a decoration — a search icon, a prefix — has its value written over
    // that decoration instead, and the symptom (a missing icon) looks nothing like the cause.
    for (const std::string& name : names) {
        const Uuid instance = ui.Place(name, { 40.0f, 40.0f });
        const ViewTree& tree = ui.host.Tree();
        for (u32 field : tree.FindAllRoles(ui.View(instance), Role::TextInput)) {
            // The widget's own rule, restated: a descendant called "Label" if there is one,
            // otherwise the first text node that is not part of the widget's menu.
            std::vector<u32> stack{ field };
            u32 chosen = ViewTree::kInvalid;
            bool named = false;
            while (!stack.empty() && !named) {
                const u32 current = stack.back();
                stack.pop_back();
                const ViewTree::View& node = tree.At(current);
                if (current != field && node.role == Role::Content) continue;
                if (node.kind == doc::NodeKind::Text) {
                    if (node.name == "Label") { chosen = current; named = true; break; }
                    if (chosen == ViewTree::kInvalid) chosen = current;
                }
                for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                    stack.push_back(*it);
            }
            CHECK_MESSAGE(chosen != ViewTree::kInvalid, name + ": its field has somewhere to show text");
            if (chosen == ViewTree::kInvalid) continue;
            const std::string& label = tree.At(chosen).name;
            CHECK_MESSAGE(label == "Text" || label == "Label",
                          name + ": a field would write its value onto '" + label + "'");
        }
        ui.document.DeleteNode(instance);
        ui.Frame();
    }

    // And the whole library paints without a device, which is what says the parts have real boxes.
    for (const std::string& name : names) ui.Place(name, { 40.0f, 40.0f });
    draw::DrawList list;
    PaintContext paint;
    paint.list = &list;
    ui.host.Paint(paint);
    CHECK(!list.Quads().empty());
}

TEST(ui, painting_records_primitives_without_a_device) {
    Ui ui;
    ui.Place("Button", { 40.0f, 40.0f });
    ui.Place("Checkbox", { 40.0f, 120.0f });
    ui.Place("Slider", { 40.0f, 200.0f });
    ui.Frame();

    draw::DrawList list;
    PaintContext paint;
    paint.list = &list;
    ui.host.Paint(paint);

    CHECK(!list.Batches().empty());
    CHECK(!list.Quads().empty());
    // No atlas was provided, so no glyph fills were recorded.
    for (const auto& quad : list.Quads())
        CHECK(quad.params.y != static_cast<f32>(draw::Paint::Kind::Glyph));
}

TEST(ui, a_theme_switch_repaints_every_widget_from_the_tokens) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    const Color dark = ui.host.Tree().Resolved(ui.View(button)).Colour(doc::Prop::Fill);

    ui.document.SetTheme(doc::Theme::Light);
    ui.Frame();
    const Color light = ui.host.Tree().Resolved(ui.View(button)).Colour(doc::Prop::Fill);
    CHECK(dark != light);
}

TEST(ui, display_scale_changes_nothing_about_the_geometry) {
    // The whole DPI model in one assertion: the document, the layout and every box in it are in
    // logical pixels, and the display's scale is applied once at the very end. A design that moved
    // when someone dragged the window onto a different monitor would be a design nobody could trust.
    Ui ui;
    ui.Place("Button", { 40.0f, 40.0f });
    ui.Place("Switch", { 40.0f, 120.0f });
    ui.Place("Slider", { 40.0f, 200.0f });
    ui.Frame();

    const auto paint = [&](f32 ratio) {
        draw::DrawList list;
        PaintContext context;
        context.list = &list;
        context.pixelRatio = ratio;     // no atlas: geometry is the question, not rasterization
        ui.host.Paint(context);
        return list.Quads();
    };

    const auto one = paint(1.0f);
    const auto two = paint(2.0f);
    CHECK(!one.empty());
    CHECK(one.size() == two.size());
    bool identical = one.size() == two.size();
    for (std::size_t i = 0; identical && i < one.size(); ++i)
        identical = std::memcmp(&one[i], &two[i], sizeof(draw::QuadInstance)) == 0;
    CHECK(identical);
}

// ------------------------------------------------------------------------------- screens

namespace {

    // Three screens and the relations between them: a list that goes to a detail, a detail that
    // goes back, and an alert presented over whichever is showing.
    struct App {
        doc::Document document;
        Library library;
        UiHost host;
        Uuid list, detail, alert;
        Vec2 size{ 800.0f, 600.0f };

        App() {
            library = BuildStandardLibrary(document);
            list   = Screen("List");
            detail = Screen("Detail");
            alert  = Screen("Confirm");
            document.SetProp(alert, doc::Prop::ScreenKind, std::string("alert"));
            // Authored smaller than the screen it is shown over, which is what a dialog is and what
            // gives "clicking outside it" a meaning.
            doc::Node* dialog = document.Find(alert);
            dialog->layout.width = layout::Size::Px(320.0f);
            dialog->layout.height = layout::Size::Px(180.0f);
            document.Touch(alert);

            document.SetStartScreen(list);

            host.SetDocument(document, document.StartScreen());
            Frame();
        }

        Uuid Screen(std::string name) {
            const Uuid id = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(),
                                                std::move(name));
            doc::Node* node = document.Find(id);
            node->layout.mode = layout::LayoutMode::Absolute;
            node->layout.width = layout::Size::Px(size.x);
            node->layout.height = layout::Size::Px(size.y);
            document.Touch(id);
            return id;
        }

        // A button on a screen, optionally with a declared destination.
        Uuid Button(Uuid screen, std::string name, Vec2 at, const char* goTo = nullptr) {
            const Uuid instance = document.CreateInstance(library.Find("Button"), screen);
            doc::Node* node = document.Find(instance);
            node->name = std::move(name);
            node->layout.offsetStart = at;
            document.Touch(instance);
            if (goTo)
                document.SetOverride(instance, library.Find("Button"), doc::Prop::GoTo,
                                     std::string(goTo));
            Frame();
            return instance;
        }

        // The real frame order: a declared destination takes effect after whatever was listening
        // has seen the click, and only then is the new screen laid out.
        void Frame(f32 dt = 1.0f / 60.0f) {
            host.ApplyNavigation();
            host.Update(size, dt);
        }

        u32 View(Uuid instance) const {
            const doc::Node* node = document.Find(instance);
            return host.ActiveTree().ViewOf(WidgetId{ node ? node->componentId : Uuid::Invalid(),
                                                      instance });
        }

        void ClickOn(Uuid instance) {
            const u32 view = View(instance);
            if (view == ViewTree::kInvalid) return;
            const Vec2 point = host.ActiveTree().Bounds(view).Center();
            host.Dispatch(MakeMouseMoved(point.x, point.y));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          point.x, point.y, Mod::None));
            host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          point.x, point.y, Mod::None));
            Frame();
        }
    };

}

TEST(ui, a_document_says_which_screen_an_app_opens_on) {
    App app;
    CHECK(app.document.StartScreen() == app.list);
    CHECK(app.host.CurrentScreenName() == "List");

    app.document.SetStartScreen(app.detail);
    CHECK(app.document.StartScreen() == app.detail);

    // Deleting the chosen one falls back rather than leaving the app with nowhere to open.
    app.document.DeleteNode(app.detail);
    CHECK(app.document.StartScreen() == app.list);
}

TEST(ui, navigating_to_a_page_replaces_the_screen_and_back_returns) {
    App app;
    CHECK(app.host.GoToScreen("Detail"));
    CHECK(app.host.CurrentScreenName() == "Detail");
    CHECK(app.host.ScreenDepth() == 1);

    CHECK(app.host.GoBack());
    CHECK(app.host.CurrentScreenName() == "List");
    CHECK(app.host.ScreenDepth() == 0);

    // Nowhere left to go, which is what a hardware back button needs to be told.
    CHECK(!app.host.GoBack());
}

TEST(ui, navigating_to_a_screen_that_does_not_exist_changes_nothing) {
    App app;
    CHECK(!app.host.GoToScreen("Nowhere"));
    CHECK(app.host.CurrentScreenName() == "List");
    // And going to the screen already showing is not a navigation either.
    CHECK(!app.host.GoToScreen("List"));
    CHECK(app.host.ScreenDepth() == 0);
}

TEST(ui, an_alert_is_presented_over_the_screen_rather_than_replacing_it) {
    App app;
    CHECK(app.host.GoToScreen("Confirm"));
    // The screen underneath stayed exactly where it was, which is what makes "close it and carry
    // on" work without the app rebuilding itself.
    CHECK(app.host.CurrentScreenName() == "List");
    CHECK(app.host.OverlayCount() == 1);
    CHECK(app.host.ScreenDepth() == 0);

    CHECK(app.host.GoBack());
    CHECK(app.host.OverlayCount() == 0);
    CHECK(app.host.CurrentScreenName() == "List");
}

TEST(ui, an_alert_blocks_what_is_underneath_and_does_not_dismiss_itself) {
    App app;
    const Uuid below = app.Button(app.list, "Underneath", { 40.0f, 40.0f });
    app.host.GoToScreen("Confirm");
    app.Frame();
    CHECK(app.host.OverlayCount() == 1);

    // A click on the scrim: an alert is a question, so it stays until it is answered.
    app.host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                      700.0f, 560.0f, Mod::None));
    app.host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                      700.0f, 560.0f, Mod::None));
    app.Frame();
    CHECK(app.host.OverlayCount() == 1);

    // And the button underneath never sees any of it.
    app.host.ClearActions();
    const Rect bounds = app.host.Tree().Bounds(app.host.Tree().ViewOf(
        WidgetId{ app.document.Find(below)->componentId, below }));
    const Vec2 point = bounds.Center();
    app.host.Dispatch(MakeMouseMoved(point.x, point.y));
    app.host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                      point.x, point.y, Mod::None));
    app.host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                      point.x, point.y, Mod::None));
    app.Frame();
    CHECK(!app.host.Fired(ActionKind::Clicked, "Underneath"));
}

TEST(ui, a_presented_screen_dims_the_whole_surface_behind_it) {
    App app;
    app.Button(app.list, "Underneath", { 40.0f, 40.0f });
    app.Frame();

    // The alert is authored at 320x180 over an 800x600 surface, and the screen behind it may itself
    // be smaller than the window. The dimming is the host's, so it covers everything either way.
    const auto dimmers = [&] {
        draw::DrawList list;
        PaintContext paint;
        paint.list = &list;
        app.host.Paint(paint);
        u32 count = 0;
        for (const auto& quad : list.Quads()) {
            if (quad.rect.x != 0.0f || quad.rect.y != 0.0f) continue;
            if (quad.rect.z != app.size.x || quad.rect.w != app.size.y) continue;
            if (quad.color0.w <= 0.0f || quad.color0.w >= 1.0f) continue;
            ++count;
        }
        return count;
    };

    CHECK(dimmers() == 0);

    app.host.GoToScreen("Confirm");
    app.Frame();
    CHECK(dimmers() == 1);

    // And it goes with the alert, rather than dimming an app nobody is blocking.
    app.host.GoBack();
    app.Frame();
    CHECK(dimmers() == 0);
}

TEST(ui, a_modal_screen_does_dismiss_itself) {
    App app;
    app.document.SetProp(app.alert, doc::Prop::ScreenKind, std::string("modal"));
    app.Frame();

    app.host.GoToScreen("Confirm");
    app.Frame();
    CHECK(app.host.OverlayCount() == 1);

    app.host.Dispatch(MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                      700.0f, 560.0f, Mod::None));
    app.host.Dispatch(MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                      700.0f, 560.0f, Mod::None));
    app.Frame();
    CHECK(app.host.OverlayCount() == 0);
}

TEST(ui, a_button_can_say_where_it_goes_without_a_script) {
    // The case that needs no logic should need no code: a designer wiring two screens together
    // should not have to open the editor.
    App app;
    app.Button(app.list, "Open", { 40.0f, 40.0f }, "Detail");
    const Uuid back = app.Button(app.detail, "Back", { 40.0f, 40.0f }, "back");

    app.ClickOn(app.document.Find(app.list)->children.front());
    CHECK(app.host.CurrentScreenName() == "Detail");

    app.ClickOn(back);
    CHECK(app.host.CurrentScreenName() == "List");
}

TEST(ui, a_declared_destination_still_emits_the_click) {
    // Navigation is not instead of the action — a script may want to save something on the way out.
    App app;
    const Uuid open = app.Button(app.list, "Open", { 40.0f, 40.0f }, "Detail");
    app.host.ClearActions();
    app.ClickOn(open);
    CHECK(app.host.Fired(ActionKind::Clicked, "Open"));
    CHECK(app.host.CurrentScreenName() == "Detail");
}

TEST(ui, back_skips_a_screen_that_was_deleted_while_it_was_on_the_stack) {
    App app;
    app.host.GoToScreen("Detail");
    app.host.GoToScreen("Confirm");   // an alert: presented, not stacked
    app.host.GoBack();                // closes the alert
    CHECK(app.host.CurrentScreenName() == "Detail");

    app.document.DeleteNode(app.list);
    app.Frame();
    // Nowhere valid to go back to, and it says so rather than navigating to a deleted screen.
    CHECK(!app.host.GoBack());
    CHECK(app.host.CurrentScreenName() == "Detail");
}

// ------------------------------------------------------------------ state reaches the parts

TEST(ui, a_part_is_restyled_by_the_state_of_the_widget_it_belongs_to) {
    Ui ui;
    const Uuid toggle = ui.Place("Switch");
    const u32 track = ui.host.Tree().FindRole(ui.View(toggle), Role::Track);
    CHECK(track != ViewTree::kInvalid);
    const Color off = ui.host.Tree().Resolved(track).Colour(doc::Prop::Fill);

    // `checked:fill` lives on the track, but Checked is a fact about the switch. A part has to see
    // its own widget's state or a designer can never restyle one.
    ui.ClickOn(toggle);
    ui.Settle();
    const u32 after = ui.host.Tree().FindRole(ui.View(toggle), Role::Track);
    CHECK(ui.host.Tree().Resolved(after).Colour(doc::Prop::Fill) != off);
}

TEST(ui, hovering_a_button_eases_its_fill_rather_than_snapping) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    const u32 view = ui.View(button);
    const Color rest = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);

    ui.Move(ui.Center(button));
    ui.Frame();
    const Color first = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);
    // One frame in, it has started but is nowhere near arrived. A snap would already be there.
    CHECK(first != rest);

    ui.Settle();
    const Color hovered = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);
    CHECK(hovered != rest);
    CHECK(first != hovered);

    // Between the two, the whole way — not past either end.
    const auto between = [](f32 value, f32 a, f32 bb) {
        const f32 low = std::min(a, bb), high = std::max(a, bb);
        return value >= low - 0.001f && value <= high + 0.001f;
    };
    CHECK(between(first.r, rest.r, hovered.r));
    CHECK(between(first.g, rest.g, hovered.g));
    CHECK(between(first.b, rest.b, hovered.b));

    // And back again when the pointer leaves.
    ui.Move({ 700.0f, 500.0f });
    ui.Settle();
    const Color back = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);
    CHECK_MESSAGE(back == rest,
                  std::to_string(back.r) + "," + std::to_string(back.g) + "," + std::to_string(back.b)
                      + " vs " + std::to_string(rest.r) + "," + std::to_string(rest.g) + ","
                      + std::to_string(rest.b) + " animating=" + (ui.host.Animating() ? "y" : "n"));
}

TEST(ui, a_recoloured_button_hovers_its_own_colour) {
    // Hover used to be a second colour the library named, so a button someone made red still
    // hovered the blue the library picked. A tint is a decision about the widget, not about a
    // particular blue, so it follows whatever the base becomes.
    Ui ui;
    ui.host.SetMotion({ false });
    const Uuid button = ui.Place("Button");
    const u32 view = ui.View(button);

    ui.Move(ui.Center(button));
    ui.Frame();
    const Color themedHover = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);
    ui.Move({ 700.0f, 500.0f });
    ui.Frame();

    const Color red{ 0.85f, 0.15f, 0.15f, 1.0f };
    ui.document.SetOverride(button, ui.ComponentRoot(button), doc::Prop::Fill, red);
    ui.Frame();
    CHECK(ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill) == red);

    ui.Move(ui.Center(button));
    ui.Frame();
    const Color hovered = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);

    CHECK(hovered != red);                      // it still reacts
    CHECK(hovered != themedHover);              // but not to the library's blue
    CHECK(hovered.r > hovered.g && hovered.r > hovered.b);   // it is a lighter red
    CHECK(hovered.r >= red.r);

    // Naming a colour outright still wins: a tint is the default, not a policy.
    const Color green{ 0.2f, 0.8f, 0.3f, 1.0f };
    ui.document.SetOverride(button, ui.ComponentRoot(button),
                            ui::StateKey(StateBit::Hovered, doc::Prop::Fill), green);
    ui.Frame();
    CHECK(ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill) == green);
}

TEST(ui, motion_can_be_turned_off_and_then_a_state_change_is_immediate) {
    // Reduced-motion is an accessibility setting, not a preference, and a test that wants to assert
    // an end state should not have to run a clock to get there.
    Ui ui;
    ui.host.SetMotion({ false, 0.14f, motion::Easing::OutCubic });
    const Uuid button = ui.Place("Button");
    const u32 view = ui.View(button);
    const Color rest = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);

    ui.Move(ui.Center(button));
    ui.Frame();
    const Color hovered = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);
    CHECK(hovered != rest);
    CHECK(!ui.host.Animating());
}

TEST(ui, an_interrupted_transition_carries_on_from_where_it_got_to) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    const u32 view = ui.View(button);
    const Color rest = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);

    ui.Move(ui.Center(button));
    ui.Frame();
    ui.Frame();
    const Color partway = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);

    // Leaving mid-fade must not snap back to rest first — that flicker is the whole reason
    // transitions are interruptible rather than restarted.
    ui.Move({ 700.0f, 500.0f });
    ui.Frame();
    const Color turning = ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill);
    CHECK(turning != rest);
    CHECK(std::abs(turning.r - partway.r) < std::abs(rest.r - partway.r) + 0.001f);

    ui.Settle();
    CHECK(ui.host.Tree().Resolved(view).Colour(doc::Prop::Fill) == rest);
}

TEST(ui, a_disabled_widget_dims_its_own_label) {
    Ui ui;
    const Uuid button = ui.Place("Button");
    const u32 label = ui.host.Tree().At(ui.View(button)).children.front();
    const Color enabled = ui.host.Tree().Resolved(label).Colour(doc::Prop::TextColor);

    ui.document.SetOverride(button, ui.ComponentRoot(button), doc::Prop::Enabled, false);
    ui.Frame();
    const u32 view = ui.host.Tree().At(ui.View(button)).children.front();
    CHECK(ui.host.Tree().Resolved(view).Colour(doc::Prop::TextColor) != enabled);
}

TEST(ui, state_does_not_leak_out_of_the_widget_it_belongs_to) {
    Ui ui;
    const Uuid a = ui.Place("Checkbox", { 40.0f, 40.0f });
    const Uuid b = ui.Place("Checkbox", { 40.0f, 120.0f });
    ui.ClickOn(a);
    ui.Frame();

    // Both checkboxes sit under the same screen; the checked one must not tint the other.
    const u32 boxB = ui.host.Tree().At(ui.View(b)).children.front();
    CHECK(!HasState(ui.host.Tree().At(boxB).state, StateBit::Checked));
    CHECK(!HasState(ui.host.Tree().At(ui.View(b)).state, StateBit::Checked));
}

// ------------------------------------------------------------------ virtualized scrolling

TEST(ui, the_wheel_scrolls_a_virtualized_list) {
    Ui ui;
    const Uuid list = ui.Place("List");
    ui.host.SetDataSource({ ui.ComponentRoot(list), list }, CreateRef<Rows>(500u));
    ui.Frame();

    ui.Move(ui.Center(list));
    ui.Wheel(-4.0f);
    ui.Frame();
    const f32 scrolled = ui.host.Tree().At(ui.View(list)).scroll.y;
    CHECK(scrolled > 0.0f);

    // And it stops at the end rather than running past a virtual content height it never measured.
    for (int i = 0; i < 500; ++i) ui.Wheel(-4.0f);
    ui.Frame();
    const f32 limit = 500.0f * 28.0f - ui.Bounds(list).size.y;
    CHECK_NEAR(ui.host.Tree().At(ui.View(list)).scroll.y, limit);
}

TEST(ui, a_virtual_lists_thumb_is_sized_from_the_rows_it_does_not_have) {
    Ui ui;
    const Uuid list = ui.Place("List");
    ui.host.SetDataSource({ ui.ComponentRoot(list), list }, CreateRef<Rows>(500u));
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    const u32 thumb = tree.FindRole(ui.View(list), Role::Thumb);
    CHECK(thumb != ViewTree::kInvalid);
    const f32 track = tree.Bounds(tree.At(thumb).parent).size.y;
    // 500 rows in a 240px window: the thumb is a sliver, not the whole track.
    CHECK(tree.Bounds(thumb).size.y < track * 0.25f);
    CHECK(tree.Bounds(thumb).size.y >= 16.0f);
}


// --------------------------------------------------------------------------- disclosure

TEST(ui, a_collapsible_opens_from_its_header_and_not_from_its_body) {
    Ui ui;
    const Uuid section = ui.Place("Collapsible");
    const ViewTree& tree = ui.host.Tree();
    const u32 root = ui.View(section);
    const u32 header = tree.FindByName("Header");
    const u32 body = tree.FindRole(root, Role::Content);
    CHECK(header != ViewTree::kInvalid && body != ViewTree::kInvalid);

    // Closed, the body takes no room at all: a collapsed section that still reserves its height
    // is a gap in the page nobody can explain.
    const f32 closed = tree.Bounds(root).size.y;
    CHECK_NEAR(closed, tree.Bounds(header).size.y);
    CHECK(!ui.Flag(section, doc::Prop::Open));

    ui.Click(tree.Bounds(header).Center());
    ui.Frame();
    CHECK(ui.Flag(section, doc::Prop::Open));
    CHECK(HasState(ui.State(section), StateBit::Open));
    CHECK(ui.host.Tree().Bounds(ui.View(section)).size.y > closed);
    CHECK(ui.host.Tree().At(ui.host.Tree().FindRole(ui.View(section), Role::Content)).visible);

    // The chevron turned because the root is open, with nothing written to the text node.
    const u32 chevron = ui.host.Tree().FindByName("Chevron");
    CHECK(chevron != ViewTree::kInvalid);
    CHECK_EQ(ui.host.Tree().Str(chevron, doc::Prop::Text), std::string("\u25be"));

    // A click in the body is the body's. This is why the header is not a Button.
    const u32 openBody = ui.host.Tree().FindRole(ui.View(section), Role::Content);
    ui.Click(ui.host.Tree().Bounds(openBody).Center());
    ui.Frame();
    CHECK(ui.Flag(section, doc::Prop::Open));

    ui.Click(ui.host.Tree().Bounds(ui.host.Tree().FindByName("Header")).Center());
    ui.Frame();
    CHECK(!ui.Flag(section, doc::Prop::Open));
    CHECK_NEAR(ui.host.Tree().Bounds(ui.View(section)).size.y, closed);
}

TEST(ui, an_accordion_keeps_exactly_one_section_open) {
    Ui ui;
    const Uuid accordion = ui.Place("Accordion");
    const ViewTree& tree = ui.host.Tree();
    const auto sections = tree.FindAllRoles(ui.View(accordion), Role::Collapsible);
    CHECK_EQ(sections.size(), std::size_t(3));

    const auto openCount = [&] {
        u32 count = 0;
        const ViewTree& now = ui.host.Tree();
        for (u32 view : now.FindAllRoles(ui.View(accordion), Role::Collapsible))
            if (now.Flag(view, doc::Prop::Open)) ++count;
        return count;
    };
    CHECK_EQ(openCount(), 1u);
    CHECK(tree.Flag(sections[0], doc::Prop::Open));

    // Opening the second closes the first, without the second knowing the first exists.
    const auto headerOf = [&](u32 section) {
        const ViewTree& now = ui.host.Tree();
        for (u32 child : now.At(section).children)
            if (now.At(child).name == "Header") return child;
        return ViewTree::kInvalid;
    };
    ui.Click(tree.Bounds(headerOf(sections[1])).Center());
    ui.Frame();

    const ViewTree& after = ui.host.Tree();
    const auto now = after.FindAllRoles(ui.View(accordion), Role::Collapsible);
    CHECK_EQ(openCount(), 1u);
    CHECK(!after.Flag(now[0], doc::Prop::Open));
    CHECK(after.Flag(now[1], doc::Prop::Open));
}

// --------------------------------------------------------------------------- feedback

TEST(ui, a_progress_bar_fills_to_its_value) {
    Ui ui;
    const Uuid bar = ui.Place("Progress");
    const auto ratio = [&] {
        const ViewTree& tree = ui.host.Tree();
        const u32 root = ui.View(bar);
        const u32 fill = tree.FindRole(root, Role::Fill);
        CHECK(fill != ViewTree::kInvalid);
        return tree.Bounds(fill).size.x / std::max(tree.Bounds(root).size.x, 1.0f);
    };
    CHECK_NEAR_EPS(ratio(), 0.6f, 0.02f);

    ui.document.SetOverride(bar, ui.ComponentRoot(bar), doc::Prop::Value, 0.25f);
    ui.Frame();
    CHECK_NEAR_EPS(ratio(), 0.25f, 0.02f);

    // Out of range on both sides, because a task that reports 130% done is a task that reports.
    ui.document.SetOverride(bar, ui.ComponentRoot(bar), doc::Prop::Value, 1.9f);
    ui.Frame();
    CHECK_NEAR_EPS(ratio(), 1.0f, 0.02f);
    ui.document.SetOverride(bar, ui.ComponentRoot(bar), doc::Prop::Value, -3.0f);
    ui.Frame();
    CHECK_NEAR_EPS(ratio(), 0.0f, 0.02f);
}

// --------------------------------------------------------------------------- split

TEST(ui, dragging_a_splitters_divider_moves_the_split) {
    Ui ui;
    const Uuid split = ui.Place("Splitter");
    const auto fraction = [&] {
        const ViewTree& tree = ui.host.Tree();
        const u32 root = ui.View(split);
        return tree.Bounds(tree.At(root).children.front()).size.x
             / std::max(tree.Bounds(root).size.x, 1.0f);
    };
    CHECK_NEAR_EPS(fraction(), 0.4f, 0.02f);

    const Rect box = ui.Bounds(split);
    const u32 divider = ui.host.Tree().FindRole(ui.View(split), Role::Knob);
    CHECK(divider != ViewTree::kInvalid);

    const Vec2 grab = ui.host.Tree().Bounds(divider).Center();
    ui.Press(grab);
    ui.Move({ box.Left() + box.size.x * 0.7f, grab.y });
    ui.Frame();
    CHECK_NEAR_EPS(fraction(), 0.7f, 0.02f);
    CHECK_NEAR_EPS(ui.Number(split, doc::Prop::Value), 0.7f, 0.02f);
    ui.Release({ box.Left() + box.size.x * 0.7f, grab.y });

    // Past the end it stops at the limit rather than collapsing a pane to nothing.
    ui.Press(ui.host.Tree().Bounds(ui.host.Tree().FindRole(ui.View(split), Role::Knob)).Center());
    ui.Move({ box.Left() + box.size.x * 2.0f, grab.y });
    ui.Frame();
    CHECK_NEAR_EPS(fraction(), 0.85f, 0.02f);
    ui.Release({ box.Right(), grab.y });

    // And a drag that starts on a pane is not a drag on the divider.
    const f32 held = fraction();
    ui.Press({ box.Left() + 20.0f, grab.y });
    ui.Move({ box.Left() + box.size.x * 0.2f, grab.y });
    ui.Frame();
    CHECK_NEAR_EPS(fraction(), held, 0.001f);
    ui.Release({ box.Left() + box.size.x * 0.2f, grab.y });
}

// --------------------------------------------------------------------------- pointer-opened

TEST(ui, a_tooltip_waits_for_a_dwell_and_closes_when_the_pointer_leaves) {
    Ui ui;
    const Uuid tip = ui.Place("Tooltip");
    const Vec2 over = ui.Center(tip);

    ui.Move(over);
    ui.Frame(0.1f);
    ui.Frame(0.1f);
    // Not yet: crossing a control on the way somewhere else must not open anything.
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));

    for (int i = 0; i < 6 && ui.host.OverlayCount() == 0; ++i) ui.Frame(0.1f);
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));
    CHECK(HasState(ui.State(tip), StateBit::Open));

    ui.Move({ 700.0f, 500.0f });
    ui.Frame(0.1f);
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
}

TEST(ui, a_right_click_opens_a_context_menu_at_the_pointer) {
    Ui ui;
    const Uuid region = ui.Place("ContextMenu");
    const Rect box = ui.Bounds(region);
    const Vec2 at{ box.Left() + 30.0f, box.Top() + 30.0f };

    // A left click is not a context menu, and the region has no other reason to react to one.
    ui.Click(at);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));

    ui.Move(at);
    ui.RightPress(at);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));
    const Vec2 origin = ui.host.OverlayAt(0).tree->Origin();
    CHECK_NEAR_EPS(origin.x, at.x, 1.0f);
    CHECK_NEAR_EPS(origin.y, at.y, 1.0f);

    const ViewTree& menu = *ui.host.OverlayAt(0).tree;
    const auto items = menu.FindAllRoles(menu.Root(), Role::DropdownItem);
    CHECK_EQ(items.size(), std::size_t(3));

    ui.Click(menu.Bounds(items[1]).Center());
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));
    CHECK(ui.Fired(ActionKind::SelectionChanged));
    CHECK_NEAR(ui.Number(region, doc::Prop::SelectedIndex), 1.0f);

    // The region is not a dropdown, so the chosen item's text does not become its label. This is
    // the difference between acting on a page and rewriting it.
    const u32 hint = ui.host.Tree().FindByName("Hint");
    CHECK(hint != ViewTree::kInvalid);
    CHECK_EQ(ui.host.Tree().Str(hint, doc::Prop::Text), std::string("Right-click here"));
}

// --------------------------------------------------------------------------- menus

TEST(ui, a_select_wears_its_choice_and_a_menu_does_not) {
    Ui ui;
    const Uuid select = ui.Place("Dropdown", { 40.0f, 40.0f });
    ui.ClickOn(select);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));
    {
        const ViewTree& menu = *ui.host.OverlayAt(0).tree;
        const auto items = menu.FindAllRoles(menu.Root(), Role::DropdownItem);
        CHECK_EQ(items.size(), std::size_t(3));
        ui.Click(menu.Bounds(items[1]).Center());
    }
    ui.Frame();
    // A select holds a value, so it shows the one that was picked.
    const u32 selectLabel = LabelIn(ui, ui.View(select));
    CHECK_EQ(ui.host.Tree().Str(selectLabel, doc::Prop::Text), std::string("Option 2"));

    const Uuid menu = ui.Place("Menu", { 400.0f, 40.0f });
    ui.ClickOn(menu);
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));
    {
        const ViewTree& sheet = *ui.host.OverlayAt(0).tree;
        const auto items = sheet.FindAllRoles(sheet.Root(), Role::DropdownItem);
        CHECK_EQ(items.size(), std::size_t(3));
        ui.Click(sheet.Bounds(items[1]).Center());
    }
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(0));

    // A menu holds actions, so it keeps its own name and reports which action was chosen.
    const u32 menuLabel = LabelIn(ui, ui.View(menu));
    CHECK_EQ(ui.host.Tree().Str(menuLabel, doc::Prop::Text), std::string("Menu"));
    bool named = false;
    for (const Action& action : ui.host.Actions())
        if (action.kind == ActionKind::Clicked && action.name == "Open") named = true;
    CHECK(named);
    CHECK_NEAR(ui.Number(menu, doc::Prop::SelectedIndex), 1.0f);
}

TEST(ui, pagination_walks_pages_and_stops_at_both_ends) {
    Ui ui;
    const Uuid pager = ui.Place("Pagination");
    const auto part = [&](std::string_view name) {
        const ViewTree& tree = ui.host.Tree();
        for (u32 child : tree.At(ui.View(pager)).children)
            if (tree.At(child).name == name) return child;
        return ViewTree::kInvalid;
    };
    const auto selected = [&] {
        const ViewTree& tree = ui.host.Tree();
        const auto pages = tree.FindAllRoles(ui.View(pager), Role::Tab);
        for (u32 i = 0; i < pages.size(); ++i)
            if (HasState(tree.At(pages[i]).state, StateBit::Selected)) return static_cast<i32>(i) + 1;
        return 0;
    };

    CHECK_NEAR(ui.Number(pager, doc::Prop::Value), 1.0f);
    CHECK_EQ(selected(), 1);
    // Page one has nowhere back to go, and the arrow says so rather than vanishing.
    CHECK(HasState(ui.host.Tree().At(part("Prev")).state, StateBit::Disabled));

    ui.Click(ui.host.Tree().Bounds(part("Next")).Center());
    ui.Frame();
    CHECK_NEAR(ui.Number(pager, doc::Prop::Value), 2.0f);
    CHECK_EQ(selected(), 2);

    ui.Click(ui.host.Tree().Bounds(part("Next")).Center());
    ui.Frame();
    ui.Click(ui.host.Tree().Bounds(part("Next")).Center());
    ui.Frame();
    // Three pages drawn and no count stated, so three is the end.
    CHECK_NEAR(ui.Number(pager, doc::Prop::Value), 3.0f);
    CHECK(HasState(ui.host.Tree().At(part("Next")).state, StateBit::Disabled));

    ui.Click(ui.host.Tree().Bounds(part("Page 1")).Center());
    ui.Frame();
    CHECK_NEAR(ui.Number(pager, doc::Prop::Value), 1.0f);
    CHECK_EQ(selected(), 1);
    CHECK(ui.Fired(ActionKind::ValueChanged));
}

// --------------------------------------------------------------------------- selectable text

TEST(ui, a_caret_lands_on_the_line_that_was_clicked) {
    Ui ui;
    // A label with the selectable box ticked, not a different component: the caret, the
    // drag-selection and Ctrl+C are the ones the field behaviour already has.
    const Uuid text = ui.Paragraph("Select this text and copy it. A read-only field is a "
                                   "paragraph you can take something out of.");
    const Rect box = ui.BoundsOfPlain(text);
    // The paragraph wraps, which is the only reason this test can tell the two answers apart.
    CHECK(box.size.y > 30.0f);

    const auto caretAfterClickAt = [&](f32 y) {
        ui.Click({ box.Left() + box.size.x - 6.0f, y });
        ui.Frame();
        const auto* edit = ui.host.FindEditState(Ui::Plain(text));
        return edit ? edit->caret : std::size_t(0);
    };

    const std::size_t firstLine = caretAfterClickAt(box.Top() + 6.0f);
    const std::size_t lastLine  = caretAfterClickAt(box.Bottom() - 6.0f);
    CHECK(firstLine > 0);
    // Laid out as one unwrapped line, every click resolves against the same run and the last line
    // answers the same offset as the first. Wrapping is what makes them different.
    CHECK(lastLine > firstLine);
}

// ------------------------------------------------------------------ one child at a time

TEST(ui, a_container_can_show_one_of_its_children_at_a_time) {
    Ui ui;
    // Four drawings of one panel — the wait, the failure, the nothing, and the thing itself.
    // Stacked in a column they would all be on screen at once, which is not a design anyone can
    // look at, and hiding three of them by hand is not a state a script can change.
    const Uuid panel = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Panel");
    {
        doc::Node* node = ui.document.Find(panel);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(300.0f);
        node->layout.height = layout::Size::Hug();
    }
    std::map<std::string, Uuid> states;
    for (const char* name : { "Loading", "Failed", "Empty", "Content" }) {
        const Uuid child = ui.document.CreateNode(doc::NodeKind::Text, panel, name);
        doc::Node* node = ui.document.Find(child);
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(40.0f);
        ui.document.SetProp(child, doc::Prop::Text, std::string(name));
        states[name] = child;
    }
    ui.Frame();

    const auto shownCount = [&] {
        const ui::ViewTree& tree = ui.host.Tree();
        int count = 0;
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (states.contains(tree.At(i).name) && tree.At(i).visible) ++count;
        return count;
    };
    const auto boxOf = [&](const char* name) {
        return ui.host.Tree().Bounds(ui.host.Tree().ViewOf(WidgetId{ states[name],
                                                                    Uuid::Invalid() }));
    };

    // Nothing named: everything shows, which is what a plain frame has always done.
    CHECK_EQ(shownCount(), 4);
    CHECK_NEAR(ui.host.Tree().Bounds(ui.host.Tree().ViewOf(WidgetId{ panel, Uuid::Invalid() })).size.y,
               160.0f);

    ui.document.SetProp(panel, doc::Prop::Shown, std::string("Loading"));
    ui.Frame();
    CHECK_EQ(shownCount(), 1);
    // The three that are not showing take no room either: a hidden alternative that still reserves
    // its height is a panel with a hole in it.
    CHECK_NEAR(ui.host.Tree().Bounds(ui.host.Tree().ViewOf(WidgetId{ panel, Uuid::Invalid() })).size.y,
               40.0f);
    CHECK_NEAR(boxOf("Loading").pos.y, 20.0f);

    // Switching is one property, which is what makes it something a script can do.
    ui.document.SetProp(panel, doc::Prop::Shown, std::string("Failed"));
    ui.Frame();
    CHECK_EQ(shownCount(), 1);
    CHECK_NEAR(boxOf("Failed").pos.y, 20.0f);

    // The ones that are not showing are still there to be filled in before they are switched to.
    CHECK(ui.host.Tree().ViewOf(WidgetId{ states["Content"], Uuid::Invalid() })
          != ui::ViewTree::kInvalid);
}

// ------------------------------------------------------------------------- repetition

TEST(ui, a_container_repeats_the_row_the_designer_styled) {
    Ui ui;
    // One row, styled once. The count is the only thing a script has to say, and it is a property
    // on the container that was already there rather than a second, data-driven List.
    const Uuid strip = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Strip");
    {
        doc::Node* node = ui.document.Find(strip);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.gap = 4.0f;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(300.0f);
        node->layout.height = layout::Size::Hug();
    }
    const Uuid row = ui.document.CreateInstance(ui.library.Find("Checkbox"), strip);
    ui.document.Find(row)->name = "Row";
    ui.document.Touch(row);
    const Uuid footer = ui.document.CreateNode(doc::NodeKind::Text, strip, "Footer");
    ui.document.SetProp(footer, doc::Prop::Text, std::string("end"));
    ui.Frame();

    const auto named = [&](std::string_view name) {
        const ui::ViewTree& tree = ui.host.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name) return i;
        return ui::ViewTree::kInvalid;
    };

    CHECK(named("Row") != ui::ViewTree::kInvalid);
    CHECK(named("Row 1") == ui::ViewTree::kInvalid);

    ui.document.SetProp(strip, doc::Prop::Repeat, 4.0f);
    ui.Frame();

    // Four of them, named for their place so a script can say which one it means, and the rest of
    // what the container held still after them.
    for (int i = 1; i <= 4; ++i)
        CHECK(named("Row " + std::to_string(i)) != ui::ViewTree::kInvalid);
    CHECK(named("Row 5") == ui::ViewTree::kInvalid);
    CHECK(named("Footer") != ui::ViewTree::kInvalid);
    CHECK(ui.host.Tree().Bounds(named("Footer")).pos.y
          > ui.host.Tree().Bounds(named("Row 4")).pos.y);

    // Stacked, not drawn on top of each other: each copy is an ordinary node in the layout.
    CHECK(ui.host.Tree().Bounds(named("Row 2")).pos.y
          > ui.host.Tree().Bounds(named("Row 1")).pos.y);

    // And each has an identity of its own, or ticking the third box would tick the first.
    const ui::ViewTree& tree = ui.host.Tree();
    CHECK(tree.At(named("Row 1")).instanceId != tree.At(named("Row 3")).instanceId);

    const Rect third = tree.Bounds(named("Row 3"));
    ui.Click(third.Center());
    ui.Frame();
    CHECK(ui.host.Tree().Number(named("Row 3"), doc::Prop::Checked, 0.0f) > 0.5f);
    CHECK(ui.host.Tree().Number(named("Row 1"), doc::Prop::Checked, 0.0f) < 0.5f);

    // Back to nothing repeated is back to what the designer drew.
    ui.document.SetProp(strip, doc::Prop::Repeat, 0.0f);
    ui.Frame();
    CHECK(named("Row") != ui::ViewTree::kInvalid);
    CHECK(named("Row 2") == ui::ViewTree::kInvalid);
}

TEST(ui, rows_handed_to_a_container_become_the_rows_it_draws) {
    Ui ui;
    // A message list, the way it is actually drawn: a scroller holding a column, holding one row
    // somebody designed. Nothing here is a List widget — the rows are frames and labels, and what
    // makes them a list is the data behind them.
    const Uuid scroller = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Scroll");
    {
        doc::Node* node = ui.document.Find(scroller);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(300.0f);
        node->layout.height = layout::Size::Px(120.0f);
        ui.document.SetProp(scroller, doc::Prop::ClipContent, true);
        ui.document.SetProp(scroller, doc::Prop::Role, std::string("scroll"));
    }
    const Uuid list = ui.document.CreateNode(doc::NodeKind::Frame, scroller, "Messages");
    {
        doc::Node* node = ui.document.Find(list);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.gap = 6.0f;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Hug();
    }
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, list, "Message");
    {
        doc::Node* node = ui.document.Find(row);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(30.0f);
    }
    const Uuid author = ui.document.CreateNode(doc::NodeKind::Text, row, "Author");
    ui.document.SetProp(author, doc::Prop::Field, std::string("author"));
    ui.document.SetProp(author, doc::Prop::FontSize, 12.0f);
    ui.Frame();

    const auto named = [&](std::string_view name) {
        const ui::ViewTree& tree = ui.host.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name) return i;
        return ui::ViewTree::kInvalid;
    };
    const auto rowsOf = [&](Uuid node) { return WidgetId{ node, Uuid::Invalid() }; };
    // A part of a copy is (name, row) away: every copy is the same node, so the name alone cannot
    // say which one, and the row number is exactly what makes it unambiguous.
    const auto part = [&](std::string_view name, i32 which) {
        const ui::ViewTree& tree = ui.host.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name && tree.At(i).row == which) return i;
        return ui::ViewTree::kInvalid;
    };

    // Nothing handed over yet: one row, exactly as drawn.
    CHECK(named("Message") != ui::ViewTree::kInvalid);
    CHECK(named("Message 1") == ui::ViewTree::kInvalid);

    doc::RowTable table;
    table.columns = { "author" };
    table.cells = { "Ada", "Grace", "Alan", "Barbara" };
    ui.host.Tree().SetRows(rowsOf(list), table);
    ui.host.MarkDirty();
    ui.Frame();

    for (int i = 1; i <= 4; ++i) CHECK(named("Message " + std::to_string(i)) != ui::ViewTree::kInvalid);
    CHECK(named("Message 5") == ui::ViewTree::kInvalid);
    CHECK(named("Message") == ui::ViewTree::kInvalid);

    // Each copy drew its own row's data, and they are stacked rather than piled up.
    const ui::ViewTree& tree = ui.host.Tree();
    const u32 first = named("Message 1");
    const u32 last = named("Message 4");
    CHECK(part("Author", 0) != ui::ViewTree::kInvalid);
    CHECK(part("Author", 3) != ui::ViewTree::kInvalid);
    CHECK(tree.Str(part("Author", 0), doc::Prop::Text) == std::string("Ada"));
    CHECK(tree.Str(part("Author", 3), doc::Prop::Text) == std::string("Barbara"));
    CHECK(tree.Bounds(last).pos.y > tree.Bounds(first).pos.y);

    // Rows survive the rebuild they caused: handing them over dirties the tree, and a table that
    // was forgotten in the rebuild would leave the list empty one frame later.
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(named("Message 4") != ui::ViewTree::kInvalid);
    CHECK(ui.host.Tree().RowsOf(rowsOf(list)) != nullptr);

    // A click anywhere inside a copy — on the label, not the row — can still say which copy.
    const u32 label = part("Author", 2);
    CHECK(label != ui::ViewTree::kInvalid);
    const u32 owner = ui.host.Tree().RowOwner(label);
    CHECK(owner != ui::ViewTree::kInvalid);
    CHECK(owner == named("Message 3"));
    CHECK_EQ(ui.host.Tree().At(owner).row, 2);
    CHECK_EQ(ui.host.Tree().RowOwner(named("Scroll")), ui::ViewTree::kInvalid);

    // Fewer rows is fewer copies, without anyone touching the document.
    doc::RowTable shorter;
    shorter.columns = { "author" };
    shorter.cells = { "Ada" };
    ui.host.Tree().SetRows(rowsOf(list), shorter);
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(named("Message 1") != ui::ViewTree::kInvalid);
    CHECK(named("Message 2") == ui::ViewTree::kInvalid);

    // And clearing them hands the container back to the designer.
    ui.host.Tree().ClearRows(rowsOf(list));
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(named("Message") != ui::ViewTree::kInvalid);
}

TEST(ui, sample_rows_draw_on_the_canvas_and_nowhere_else) {
    Ui ui;
    // The gap this closes: a designer styling a row template used to be looking at the same row
    // N times, with every column they had bound showing nothing at all.
    const Uuid list = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Messages");
    {
        doc::Node* node = ui.document.Find(list);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(300.0f);
        node->layout.height = layout::Size::Hug();
    }
    ui.document.SetProp(list, doc::Prop::Repeat, 2.0f);
    ui.document.SetProp(list, doc::Prop::Sample,
                        std::string("author | body\nAda | Hello there\nGrace | Hi\nAlan | Morning\n"));
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, list, "Message");
    {
        doc::Node* node = ui.document.Find(row);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(24.0f);
    }
    const Uuid author = ui.document.CreateNode(doc::NodeKind::Text, row, "Author");
    ui.document.SetProp(author, doc::Prop::Field, std::string("author"));
    ui.Frame();

    const ui::ViewTree& tree = ui.host.Tree();
    const auto named = [&](std::string_view name) {
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name) return i;
        return ui::ViewTree::kInvalid;
    };
    const auto part = [&](std::string_view name, i32 which) {
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name && tree.At(i).row == which) return i;
        return ui::ViewTree::kInvalid;
    };

    // Off unless asked: this is what the player and an exported app do, and neither should ever
    // put invented names on screen.
    CHECK(!tree.ShowingSampleRows());
    CHECK(named("Message 2") != ui::ViewTree::kInvalid);
    CHECK(named("Message 3") == ui::ViewTree::kInvalid);          // the authored count, not the table
    CHECK(tree.Str(part("Author", 0), doc::Prop::Text).empty());  // and no data behind it

    ui.host.Tree().ShowSampleRows(true);
    ui.Frame();
    // Three sample rows, and each copy drew its own.
    CHECK(named("Message 3") != ui::ViewTree::kInvalid);
    CHECK(named("Message 4") == ui::ViewTree::kInvalid);
    CHECK(ui.host.Tree().Str(part("Author", 0), doc::Prop::Text) == std::string("Ada"));
    CHECK(ui.host.Tree().Str(part("Author", 2), doc::Prop::Text) == std::string("Alan"));

    // Real rows still win, on the canvas as much as anywhere: pressing Play does not leave a
    // designer's placeholder people in a running app.
    doc::RowTable real;
    real.columns = { "author" };
    real.cells = { "you" };
    ui.host.Tree().SetRows(WidgetId{ list, Uuid::Invalid() }, real);
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(named("Message 1") != ui::ViewTree::kInvalid);
    CHECK(named("Message 2") == ui::ViewTree::kInvalid);
    CHECK(ui.host.Tree().Str(part("Author", 0), doc::Prop::Text) == std::string("you"));
}

TEST(ui, a_table_left_on_a_container_that_does_not_repeat_changes_nothing) {
    Ui ui;
    // Sample rows are what a repeat draws. A frame that someone typed a table onto and then
    // turned the repeat off must not quietly start copying itself.
    const Uuid card = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Card");
    ui.document.SetProp(card, doc::Prop::Sample, std::string("body\none\ntwo\n"));
    ui.document.CreateNode(doc::NodeKind::Text, card, "Label");
    ui.host.Tree().ShowSampleRows(true);
    ui.Frame();

    const ui::ViewTree& tree = ui.host.Tree();
    u32 labels = 0;
    for (u32 i = 0; i < tree.ViewCount(); ++i)
        if (tree.At(i).name.rfind("Label", 0) == 0) ++labels;
    CHECK_EQ(labels, 1u);
}

TEST(ui, the_selected_row_is_the_copy_that_lights_up) {
    Ui ui;
    const Uuid list = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Channels");
    {
        doc::Node* node = ui.document.Find(list);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(200.0f);
        node->layout.height = layout::Size::Hug();
    }
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, list, "Channel");
    {
        doc::Node* node = ui.document.Find(row);
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(28.0f);
    }
    // The one thing the designer says about selection: what a selected row looks like.
    ui.document.SetProp(row, doc::Prop::Fill, Color{ 0.1f, 0.1f, 0.1f, 1.0f });
    ui.document.Find(row)->props.Set("selected:fill", Color{ 0.9f, 0.2f, 0.2f, 1.0f });
    ui.document.Touch(row);

    doc::RowTable table;
    table.columns = { "name" };
    table.cells = { "general", "design", "engine" };
    ui.host.Tree().SetRows(WidgetId{ list, Uuid::Invalid() }, table);
    ui.document.SetProp(list, doc::Prop::SelectedIndex, 1.0f);
    ui.host.MarkDirty();
    ui.Settle();

    const auto named = [&](std::string_view name) {
        const ui::ViewTree& tree = ui.host.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).name == name) return i;
        return ui::ViewTree::kInvalid;
    };

    const ui::ViewTree& tree = ui.host.Tree();
    CHECK(HasState(tree.At(named("Channel 2")).state, StateBit::Selected));
    CHECK(!HasState(tree.At(named("Channel 1")).state, StateBit::Selected));
    CHECK(!HasState(tree.At(named("Channel 3")).state, StateBit::Selected));

    // And the overlay the designer drew is what it is painted with, which is the whole point of
    // saying it in the document rather than in a script.
    const doc::Value fill = tree.ResolvedProp(named("Channel 2"), doc::Prop::Fill);
    CHECK(std::holds_alternative<Color>(fill));
    if (std::holds_alternative<Color>(fill)) CHECK(std::get<Color>(fill).r > 0.5f);

    ui.document.SetProp(list, doc::Prop::SelectedIndex, 2.0f);
    ui.host.MarkDirty();
    ui.Settle();
    CHECK(HasState(ui.host.Tree().At(named("Channel 3")).state, StateBit::Selected));
    CHECK(!HasState(ui.host.Tree().At(named("Channel 2")).state, StateBit::Selected));
}

TEST(ui, a_container_that_fills_from_the_end_holds_short_content_against_the_bottom) {
    Ui ui;
    // A chat log: three messages should sit above the composer, not float under the title. This
    // used to be `justify: end`, which is the same picture right up until the conversation is
    // longer than the box.
    const Uuid log = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Log");
    {
        doc::Node* node = ui.document.Find(log);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(300.0f);
        node->layout.height = layout::Size::Px(200.0f);
        ui.document.SetProp(log, doc::Prop::ClipContent, true);
        ui.document.SetProp(log, doc::Prop::Role, std::string("scroll"));
        ui.document.SetProp(log, doc::Prop::StickToEnd, true);
    }
    const auto message = [&](const char* name) {
        const Uuid id = ui.document.CreateNode(doc::NodeKind::Frame, log, name);
        doc::Node* node = ui.document.Find(id);
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(40.0f);
        return id;
    };
    const Uuid first = message("One");
    message("Two");
    const Uuid third = message("Three");
    ui.Frame();

    const auto view = [&](Uuid node) {
        return ui.host.Tree().ViewOf(WidgetId{ node, Uuid::Invalid() });
    };
    const ui::ViewTree& tree = ui.host.Tree();
    const Rect box = tree.Bounds(view(log));

    // 120 of content in a 200 box: the last message ends on the bottom edge, not 80 above it.
    CHECK(std::abs(tree.Bounds(view(third)).Bottom() - box.Bottom()) < 0.5f);
    CHECK(tree.Bounds(view(first)).pos.y > box.pos.y);
    // And the content is still 120 tall — the offset that moved it must not measure as content,
    // or it would report itself as scrollable and the bar would appear out of nowhere.
    CHECK(std::abs(tree.ContentSize(view(log)).y - 120.0f) < 0.5f);
    CHECK_EQ(tree.At(view(log)).scroll.y, 0.0f);
}

TEST(ui, filling_from_the_end_becomes_scrolling_once_the_content_outgrows_the_box) {
    Ui ui;
    const Uuid log = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Log");
    {
        doc::Node* node = ui.document.Find(log);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(300.0f);
        node->layout.height = layout::Size::Px(100.0f);
        ui.document.SetProp(log, doc::Prop::ClipContent, true);
        ui.document.SetProp(log, doc::Prop::Role, std::string("scroll"));
        ui.document.SetProp(log, doc::Prop::StickToEnd, true);
    }
    const auto message = [&](const std::string& name) {
        const Uuid id = ui.document.CreateNode(doc::NodeKind::Frame, log, name);
        doc::Node* node = ui.document.Find(id);
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(40.0f);
        ui.document.Touch(id);
        return id;
    };
    for (int i = 1; i <= 4; ++i) message("Message " + std::to_string(i));
    ui.Frame();

    const auto view = [&](Uuid node) {
        return ui.host.Tree().ViewOf(WidgetId{ node, Uuid::Invalid() });
    };
    // 160 of content in a 100 box: scrolled to the end of itself, showing the newest.
    CHECK(std::abs(ui.host.Tree().At(view(log)).scroll.y - 60.0f) < 0.5f);
    CHECK(std::abs(ui.host.Tree().Bounds(view(log)).Bottom()
                   - ui.host.Tree().Bounds(view(ui.document.Find(log)->children.back())).Bottom()) < 0.5f);

    // A message arrives while the reader is at the bottom: they follow it.
    message("Message 5");
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(std::abs(ui.host.Tree().At(view(log)).scroll.y - 100.0f) < 0.5f);

    // A reader who has scrolled up stays where they are when the next one lands. Being yanked to
    // the bottom mid-sentence is the bug every chat client has had at least once.
    ui.host.Tree().SetScroll(view(log), { 0.0f, 20.0f });
    message("Message 6");
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(std::abs(ui.host.Tree().At(view(log)).scroll.y - 20.0f) < 0.5f);

    // Back to the bottom, and it follows again.
    ui.host.Tree().SetScroll(view(log), { 0.0f, 140.0f });
    message("Message 7");
    ui.host.MarkDirty();
    ui.Frame();
    CHECK(std::abs(ui.host.Tree().At(view(log)).scroll.y - 180.0f) < 0.5f);
}

TEST(ui, a_scroller_told_to_stay_at_the_end_waits_for_the_rows_that_move_it) {
    Ui ui;
    const Uuid scroller = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Scroll");
    {
        doc::Node* node = ui.document.Find(scroller);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(200.0f);
        node->layout.height = layout::Size::Px(100.0f);
        ui.document.SetProp(scroller, doc::Prop::ClipContent, true);
        ui.document.SetProp(scroller, doc::Prop::Role, std::string("scroll"));
    }
    const Uuid list = ui.document.CreateNode(doc::NodeKind::Frame, scroller, "Messages");
    {
        doc::Node* node = ui.document.Find(list);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Hug();
    }
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, list, "Message");
    {
        doc::Node* node = ui.document.Find(row);
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(40.0f);
    }

    const auto fill = [&](u32 count) {
        doc::RowTable table;
        table.columns = { "body" };
        for (u32 i = 0; i < count; ++i) table.cells.push_back("line " + std::to_string(i));
        ui.host.Tree().SetRows(WidgetId{ list, Uuid::Invalid() }, table);
    };
    const auto scrollView = [&] { return ui.host.Tree().ViewOf(WidgetId{ scroller, Uuid::Invalid() }); };

    fill(5);
    ui.host.MarkDirty();
    ui.Frame();
    CHECK_NEAR(ui.host.Tree().At(scrollView()).scroll.y, 0.0f);

    // Told at the same moment the rows arrive. The rows that decide where the end *is* have not
    // been laid out yet, so a scroller that obeyed immediately would land at the old end.
    fill(20);
    ui.host.Tree().KeepAtEnd(WidgetId{ scroller, Uuid::Invalid() });
    ui.host.MarkDirty();
    ui.Frame();

    const u32 view = scrollView();
    const f32 limit = ui.host.Tree().ContentSize(view).y - ui.host.Tree().Bounds(view).size.y;
    CHECK(limit > 0.0f);
    CHECK_NEAR(ui.host.Tree().At(view).scroll.y, limit);

    // One request, not a lock: the user can scroll back up and stay there.
    ui.host.Tree().SetScroll(view, { 0.0f, 0.0f });
    ui.Frame();
    CHECK_NEAR(ui.host.Tree().At(scrollView()).scroll.y, 0.0f);
}

// --------------------------------------------------------------------------- slots

TEST(ui, an_instance_puts_its_own_children_in_the_components_slot) {
    Ui ui;
    const Uuid card = ui.Place("Card");
    // Nothing supplied yet, and the card still looks like a card: its header is the component's,
    // and only the body is the designer's to fill.
    CHECK(ui.host.Tree().FindByName("Title") != ViewTree::kInvalid);
    CHECK(ui.host.Tree().FindByName("Body") != ViewTree::kInvalid);

    // A button dropped into the card. It is a child of the instance in the document; on screen it
    // lands inside the card's Body, which is what makes a container a container.
    const Uuid button = ui.document.CreateInstance(ui.library.Find("Button"), card);
    ui.document.Find(button)->name = "Sign in";
    ui.document.Touch(button);
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    const u32 placed = tree.ViewOf(WidgetId{ ui.library.Find("Button"), button });
    CHECK(placed != ViewTree::kInvalid);
    if (placed == ViewTree::kInvalid) return;

    u32 body = ViewTree::kInvalid;
    for (u32 at = tree.At(placed).parent; at != ViewTree::kInvalid; at = tree.At(at).parent)
        if (tree.At(at).name == "Body") { body = at; break; }
    CHECK(body != ViewTree::kInvalid);
    // And it is inside the card, not beside it.
    CHECK(ui.Bounds(card).Contains(tree.Bounds(placed).Center()));
}

TEST(ui, slot_content_belongs_to_the_page_and_not_to_the_component) {
    Ui ui;
    const Uuid first = ui.Place("Card", { 40.0f, 40.0f });
    const Uuid second = ui.Place("Card", { 40.0f, 400.0f });

    const Uuid master = ui.library.Find("Button");
    const Uuid caption = ui.document.Find(master)->children.front();
    const Uuid a = ui.document.CreateInstance(master, first);
    const Uuid b = ui.document.CreateInstance(master, second);
    ui.document.SetOverride(a, caption, doc::Prop::Text, std::string("First"));
    ui.document.SetOverride(b, caption, doc::Prop::Text, std::string("Second"));
    ui.Frame();

    // Two cards, two buttons, two captions. If slot content resolved against the component the
    // second override would win for both, which is the bug this whole scoping exists to avoid.
    const ViewTree& tree = ui.host.Tree();
    const auto textOf = [&](Uuid instance) {
        const u32 view = tree.ViewOf(WidgetId{ master, instance });
        return view == ViewTree::kInvalid ? std::string{} : tree.Str(LabelIn(ui, view), doc::Prop::Text);
    };
    CHECK_EQ(textOf(a), std::string("First"));
    CHECK_EQ(textOf(b), std::string("Second"));
}

TEST(ui, a_component_that_is_its_own_slot_replaces_its_cells) {
    Ui ui;
    const Uuid grid = ui.Place("Grid");
    CHECK(ui.host.Tree().FindByName("Cell 1") != ViewTree::kInvalid);

    for (int i = 0; i < 3; ++i) {
        const Uuid cell = ui.document.CreateInstance(ui.library.Find("Badge"), grid);
        ui.document.Find(cell)->name = "Tag " + std::to_string(i);
        ui.document.Touch(cell);
    }
    ui.Frame();

    // The placeholder is gone, not sitting behind what was supplied.
    CHECK(ui.host.Tree().FindByName("Cell 1") == ViewTree::kInvalid);
    CHECK(ui.host.Tree().FindByName("Tag 0") != ViewTree::kInvalid);
    CHECK(ui.host.Tree().FindByName("Tag 2") != ViewTree::kInvalid);
}

TEST(ui, slot_content_survives_a_save_and_a_load) {
    Ui ui;
    const Uuid card = ui.Place("Card");
    const Uuid button = ui.document.CreateInstance(ui.library.Find("Button"), card);
    ui.document.Find(button)->name = "Inside";
    ui.document.Touch(button);
    ui.Frame();

    const std::string xml = doc::Serializer::ToXml(ui.document, true, nullptr, true);
    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(xml, loaded, &error), error);

    const doc::Node* inside = loaded.Find(button);
    CHECK(inside != nullptr);
    CHECK(inside && inside->parent == card);
    // The slot flag itself has to survive, or the reloaded card drops what was put in it.
    const doc::Node* instance = loaded.Find(card);
    CHECK(instance != nullptr);
    CHECK(instance && loaded.SlotOf(instance->componentId).Valid());
}

TEST(ui, a_spinner_goes_round_and_keeps_the_frame_loop_awake) {
    Ui ui;
    const Uuid spinner = ui.Place("Spinner");
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    const std::vector<u32> dots = tree.FindAllRoles(ui.View(spinner), Role::Indicator);
    CHECK_EQ(dots.size(), std::size_t(8));

    // Round the circle, not stacked on top of each other. Their centres sit on a ring, so no two
    // of them are in the same place and all of them are inside the widget.
    const Rect box = ui.Bounds(spinner);
    std::vector<Vec2> centres;
    for (const u32 dot : dots) {
        const Rect at = tree.Bounds(dot);
        CHECK(box.Contains(at.Center()));
        for (const Vec2 other : centres)
            CHECK(std::abs(other.x - at.Center().x) + std::abs(other.y - at.Center().y) > 1.0f);
        centres.push_back(at.Center());
    }

    // And the bright one moves round, which is the whole of what a spinner does.
    const auto brightest = [&] {
        u32 best = dots.front();
        f32 most = -1.0f;
        for (const u32 dot : dots)
            if (const f32 alpha = ui.host.Tree().Number(dot, doc::Prop::Opacity, 0.0f);
                alpha > most) { most = alpha; best = dot; }
        return best;
    };
    const u32 first = brightest();
    for (int i = 0; i < 6; ++i) ui.Frame(0.05f);
    CHECK(brightest() != first);

    // Nothing is easing and no state changed, so without a behavior asking for it an idle app
    // would sleep and the one widget whose whole job is to move would stop.
    CHECK(ui.host.Animating());
}

TEST(ui, a_progress_bar_with_a_lap_time_travels_and_stays_on_its_track) {
    Ui ui;
    // The same widget and the same behaviour: a duration is how a bar says it does not know how
    // far along it is, which is the moment it stops being a measurement and starts being a wait.
    const Uuid bar = ui.Place("Progress");
    ui.document.SetOverride(bar, ui.ComponentRoot(bar), doc::Prop::Duration, 1.2f);
    ui.Frame();

    const auto barX = [&] {
        const ViewTree& tree = ui.host.Tree();
        const u32 fill = tree.FindRole(ui.View(bar), Role::Fill);
        CHECK(fill != ViewTree::kInvalid);
        return fill == ViewTree::kInvalid ? 0.0f : tree.Bounds(fill).pos.x;
    };

    const f32 start = barX();
    for (int i = 0; i < 6; ++i) ui.Frame(0.05f);
    CHECK(barX() > start);
    CHECK(ui.host.Animating());

    const Rect box = ui.Bounds(bar);
    for (int i = 0; i < 40; ++i) {
        ui.Frame(0.05f);
        const f32 x = barX();
        CHECK_MESSAGE(x > box.Left() - box.size.x && x < box.Right(), "the bar stays on its track");
    }
}

TEST(ui, a_determinate_progress_does_not_ask_to_keep_animating) {
    Ui ui;
    ui.Place("Progress");
    for (int i = 0; i < 4; ++i) ui.Frame(0.05f);
    CHECK(!ui.host.Animating());
}

TEST(ui, up_and_down_move_the_caret_by_visual_line) {
    Ui ui;
    const Uuid text = ui.Paragraph("Select this text and copy it. A read-only field is a "
                                   "paragraph you can take something out of.");
    const auto caret = [&] {
        const auto* edit = ui.host.FindEditState(Ui::Plain(text));
        return edit ? edit->caret : std::size_t(0);
    };

    // Start on the first line, a little in from the left.
    const Rect box = ui.BoundsOfPlain(text);
    CHECK(box.size.y > 30.0f);
    ui.Click({ box.Left() + 60.0f, box.Top() + 6.0f });
    ui.Frame();
    const std::size_t first = caret();
    CHECK(first > 0);

    ui.host.Focus(ui.ViewOfPlain(text));
    ui.Key(Key::Down);
    ui.Frame();
    const std::size_t second = caret();
    // The paragraph has no newlines in it at all: without visual lines Down has nowhere to go.
    CHECK(second > first);

    ui.Key(Key::Up);
    ui.Frame();
    CHECK(caret() <= second);

    // Home and End are the visual line's ends, not the paragraph's.
    ui.Key(Key::Down);
    ui.Frame();
    ui.Key(Key::Home);
    ui.Frame();
    const std::size_t lineStart = caret();
    CHECK(lineStart > 0);
    ui.Key(Key::End);
    ui.Frame();
    CHECK(caret() > lineStart);
    CHECK(caret() < ui.host.Tree().Str(ui.ViewOfPlain(text), doc::Prop::Text).size());

    // Past the top is the start of the text, which is what every editor does.
    for (int i = 0; i < 10; ++i) { ui.Key(Key::Up); ui.Frame(); }
    CHECK_EQ(caret(), std::size_t(0));
}

// --------------------------------------------------------------------------- chart

TEST(ui, a_chart_plots_the_numbers_on_its_own_node) {
    Ui ui;
    const Uuid chart = ui.Place("Chart");
    const Uuid master = ui.ComponentRoot(chart);

    const auto quads = [&] {
        draw::DrawList list;
        PaintContext paint;
        paint.list = &list;
        ui.host.Paint(paint);
        return list.Quads().size();
    };

    // The numbers live on the document, so a chart draws before anything runs — which is the whole
    // reason a designer can lay one out at all.
    const std::size_t drawn = quads();
    CHECK(drawn > 20);

    ui.document.SetOverride(chart, master, doc::Prop::Series, std::string(""));
    ui.Frame();
    const std::size_t empty = quads();
    CHECK(empty < drawn);

    // Bars draws one box per value; a line draws none of them, so the counts have to differ.
    ui.document.SetOverride(chart, master, doc::Prop::Series, std::string("1, 2, 3, 4, 5, 6"));
    ui.document.SetOverride(chart, master, doc::Prop::ChartKind, std::string("bars"));
    ui.Frame();
    const std::size_t bars = quads();
    ui.document.SetOverride(chart, master, doc::Prop::ChartKind, std::string("line"));
    ui.Frame();
    CHECK(bars > quads());

    // Commas or spaces, and a malformed tail stops rather than throwing the whole series away.
    ui.document.SetOverride(chart, master, doc::Prop::Series, std::string("3 7 5 9"));
    ui.document.SetOverride(chart, master, doc::Prop::ChartKind, std::string("bars"));
    ui.Frame();
    const std::size_t spaced = quads();
    ui.document.SetOverride(chart, master, doc::Prop::Series, std::string("3, 7, 5, 9"));
    ui.Frame();
    CHECK_EQ(quads(), spaced);
}

// --------------------------------------------------------------------------- richer input

TEST(ui, a_one_time_code_fills_one_box_per_character) {
    Ui ui;
    const Uuid otp = ui.Place("InputOtp");
    const ViewTree& tree = ui.host.Tree();
    const auto boxes = tree.At(ui.View(otp)).children;
    CHECK_EQ(boxes.size(), std::size_t(6));

    const auto digit = [&](std::size_t index) {
        const ViewTree& now = ui.host.Tree();
        const auto& cells = now.At(ui.View(otp)).children;
        const u32 label = now.FindByName("Digit");
        (void)label;
        std::vector<u32> stack{ cells[index] };
        while (!stack.empty()) {
            const u32 at = stack.back();
            stack.pop_back();
            if (now.At(at).kind == doc::NodeKind::Text) return now.Str(at, doc::Prop::Text);
            for (u32 child : now.At(at).children) stack.push_back(child);
        }
        return std::string{};
    };

    ui.ClickOn(otp);
    ui.Frame();
    ui.Type("a1b");
    ui.Frame();
    // Upper-cased on the way in: a code is read off a screen and typed back, and case is noise.
    CHECK_EQ(ui.Str(otp, doc::Prop::Text), std::string("A1B"));
    CHECK_EQ(digit(0), std::string("A"));
    CHECK_EQ(digit(2), std::string("B"));
    CHECK_EQ(digit(3), std::string(""));

    // Punctuation is not part of a code, so it does not consume a box.
    ui.Type("-!");
    ui.Frame();
    CHECK_EQ(ui.Str(otp, doc::Prop::Text), std::string("A1B"));

    ui.Key(Key::Backspace);
    ui.Frame();
    CHECK_EQ(ui.Str(otp, doc::Prop::Text), std::string("A1"));

    // Filling the last box submits: nobody presses Enter after a code.
    ui.Type("2345");
    ui.Frame();
    CHECK_EQ(ui.Str(otp, doc::Prop::Text), std::string("A12345"));
    CHECK(ui.Fired(ActionKind::Submitted));

    // And it stops there rather than scrolling a seventh character in.
    ui.Type("9");
    ui.Frame();
    CHECK_EQ(ui.Str(otp, doc::Prop::Text), std::string("A12345"));
}

TEST(ui, a_carousel_slides_and_stops_at_both_ends) {
    Ui ui;
    const Uuid carousel = ui.Place("Carousel");
    const auto trackX = [&] {
        const ViewTree& tree = ui.host.Tree();
        const u32 track = tree.FindRole(ui.View(carousel), Role::Content);
        CHECK(track != ViewTree::kInvalid);
        return track == ViewTree::kInvalid ? 0.0f : tree.Bounds(track).pos.x;
    };
    const auto part = [&](std::string_view name) {
        const ViewTree& tree = ui.host.Tree();
        for (u32 child : tree.At(ui.View(carousel)).children)
            if (tree.At(child).name == name) return child;
        return ViewTree::kInvalid;
    };

    const f32 start = trackX();
    CHECK(HasState(ui.host.Tree().At(part("Prev")).state, StateBit::Disabled));

    ui.Click(ui.host.Tree().Bounds(part("Next")).Center());
    ui.Frame();
    CHECK_NEAR(ui.Number(carousel, doc::Prop::SelectedIndex), 1.0f);
    CHECK(trackX() < start - 100.0f);

    ui.Click(ui.host.Tree().Bounds(part("Next")).Center());
    ui.Frame();
    ui.Click(ui.host.Tree().Bounds(part("Next")).Center());
    ui.Frame();
    // Three slides, so the third is the end and the arrow says so.
    CHECK_NEAR(ui.Number(carousel, doc::Prop::SelectedIndex), 2.0f);
    CHECK(HasState(ui.host.Tree().At(part("Next")).state, StateBit::Disabled));

    ui.Click(ui.host.Tree().Bounds(part("Prev")).Center());
    ui.Frame();
    CHECK_NEAR(ui.Number(carousel, doc::Prop::SelectedIndex), 1.0f);
}

TEST(ui, a_combobox_opens_on_focus_and_narrows_as_you_type) {
    Ui ui;
    const Uuid combo = ui.Place("Combobox");
    const u32 field = ui.host.Tree().FindRole(ui.View(combo), Role::TextInput);
    CHECK(field != ViewTree::kInvalid);

    ui.Click(ui.host.Tree().Bounds(field).Center());
    ui.Frame();
    CHECK_EQ(ui.host.OverlayCount(), std::size_t(1));

    const auto showing = [&] {
        if (ui.host.OverlayCount() == 0) return std::size_t(0);
        const ViewTree& menu = *ui.host.OverlayAt(0).tree;
        std::size_t count = 0;
        for (u32 item : menu.FindAllRoles(menu.Root(), Role::DropdownItem))
            if (menu.At(item).visible) ++count;
        return count;
    };
    CHECK_EQ(showing(), std::size_t(5));

    ui.Type("re");
    ui.Frame();
    // "Remix" only — the match is on the row's own text, not on where it sits in the list.
    CHECK_EQ(showing(), std::size_t(1));

    ui.Type("zzz");
    ui.Frame();
    CHECK_EQ(showing(), std::size_t(0));
}

TEST(ui, a_calendar_lays_out_a_real_month_and_picks_a_day) {
    Ui ui;
    const Uuid calendar = ui.Place("Calendar");
    // March 2026 starts on a Sunday and has 31 days, so the grid is exactly the first 31 cells.
    ui.document.SetOverride(calendar, ui.ComponentRoot(calendar), doc::Prop::Text,
                            std::string("2026-03-15"));
    ui.Frame();

    const ViewTree& tree = ui.host.Tree();
    const auto cells = tree.FindAllRoles(ui.View(calendar), Role::Tab);
    CHECK_EQ(cells.size(), std::size_t(42));

    const auto dayOf = [&](std::size_t index) {
        return static_cast<int>(ui.host.Tree().Number(
            ui.host.Tree().FindAllRoles(ui.View(calendar), Role::Tab)[index],
            doc::Prop::Value, -1.0f));
    };
    CHECK_EQ(dayOf(0), 1);
    CHECK_EQ(dayOf(30), 31);
    CHECK_EQ(dayOf(31), 0);   // past the end of the month, and blank

    // The selected day is marked, and it is the one the text names.
    CHECK(HasState(tree.At(cells[14]).state, StateBit::Selected));

    // The title says which month is showing.
    const u32 title = tree.FindByName("Title");
    CHECK(title != ViewTree::kInvalid);

    // Picking a day writes the date back in ISO order, which is the one spelling that sorts.
    ui.Click(tree.Bounds(cells[19]).Center());
    ui.Frame();
    CHECK_EQ(ui.Str(calendar, doc::Prop::Text), std::string("2026-03-20"));
    CHECK(ui.Fired(ActionKind::ValueChanged));

    // And a blank cell is not a day.
    ui.Click(ui.host.Tree().Bounds(
        ui.host.Tree().FindAllRoles(ui.View(calendar), Role::Tab)[40]).Center());
    ui.Frame();
    CHECK_EQ(ui.Str(calendar, doc::Prop::Text), std::string("2026-03-20"));
}

// --------------------------------------------------- the catalog is named, not copied

TEST(library, the_standard_catalog_is_not_written_into_the_file) {
    // The whole point: 53 components and ~480 nodes are compiled into the binary, so a screen file
    // holds the screen. Before this, they were 89% of every document.
    doc::Document document;
    const Library library = BuildStandardLibrary(document);
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    document.CreateInstance(library.Find("Button"), screen);

    const std::string named = doc::Serializer::ToXml(document, true, &StandardLibrary(), true);
    const std::string copied = doc::Serializer::ToXml(document, true, nullptr, true);
    CHECK(named.size() * 10 < copied.size());
    CHECK(named.find("library=\"vae.std@") != std::string::npos);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(named, loaded, &error, &StandardLibrary()), error);
    // Same ids, so the instance still points at a Button that exists.
    const Uuid button = library.Find("Button");
    CHECK(loaded.Find(button) != nullptr);
    CHECK(loaded.Find(screen) != nullptr);
    CHECK_EQ(loaded.NodeCount(), document.NodeCount());
}

TEST(library, format_3_names_the_catalog_too) {
    // The same saving, through the markup codec: a document says which library it uses and lets the
    // binary rebuild it, rather than carrying 53 components it did not write.
    doc::Document document;
    const Library library = BuildStandardLibrary(document);
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    document.CreateInstance(library.Find("Button"), screen);

    const std::string named = doc::Serializer::ToXml(document, true, &StandardLibrary());
    const std::string copied = doc::Serializer::ToXml(document);
    CHECK(named.size() * 10 < copied.size());
    CHECK(named.find("library=\"vae.std@") != std::string::npos);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(named, loaded, &error, &StandardLibrary()), error);
    CHECK(loaded.Find(library.Find("Button")) != nullptr);
    CHECK_EQ(loaded.NodeCount(), document.NodeCount());

    // A document that needs a library and is handed none says so rather than half-loading.
    doc::Document orphan;
    std::string orphanError;
    CHECK(!doc::Serializer::FromXml(named, orphan, &orphanError));
    CHECK(orphanError.find("vae.std") != std::string::npos);
}

TEST(library, the_librarys_own_tokens_are_not_written_either) {
    // Same rule as the catalog, applied to the theme: a project that left the tokens alone gets
    // them back from the library, so an untouched document has no <tokens> block at all.
    doc::Document document;
    const Library library = BuildStandardLibrary(document);
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    document.CreateInstance(library.Find("Button"), screen);

    const std::string untouched = doc::Serializer::ToXml(document, true, &StandardLibrary());
    CHECK(untouched.find("<tokens>") == std::string::npos);
    CHECK(doc::Serializer::ToXml(document).find("<token name=\"accent\"") != std::string::npos);

    // Recolour one and the file carries that one, and only that one.
    document.SetToken("accent", doc::Token{ Color{ 0.9f, 0.2f, 0.1f, 1.0f },
                                            Color{ 0.9f, 0.2f, 0.1f, 1.0f } });
    const std::string recoloured = doc::Serializer::ToXml(document, true, &StandardLibrary());
    CHECK(recoloured.find("<token name=\"accent\"") != std::string::npos);
    CHECK(recoloured.find("<token name=\"surface\"") == std::string::npos);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(recoloured, loaded, &error, &StandardLibrary()), error);
    // The changed one is what the file said; the other twelve came back from the library.
    CHECK(loaded.Tokens() == document.Tokens());
}

TEST(library, a_deleted_default_token_stays_deleted) {
    // The one case silence cannot cover: not writing a token means "unchanged", and Install would
    // hand a deleted one straight back. So a deletion is written as one.
    doc::Document document;
    BuildStandardLibrary(document);
    document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    document.RemoveToken("danger");

    const std::string xml = doc::Serializer::ToXml(document, true, &StandardLibrary());
    CHECK(xml.find("name=\"danger\" removed=\"true\"") != std::string::npos);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(xml, loaded, &error, &StandardLibrary()), error);
    CHECK(loaded.FindToken("danger") == nullptr);
    CHECK(loaded.FindToken("accent") != nullptr);
    CHECK(loaded.Tokens() == document.Tokens());
}

TEST(library, a_format_3_document_that_inlines_the_catalog_is_folded_on_load) {
    // Written with no library, so every component is in the file; read with one, so they fold back
    // down to a reference. Without this the saving would only apply to files written from here on.
    doc::Document document;
    const Library library = BuildStandardLibrary(document);
    const Uuid screen = document.CreateNode(doc::NodeKind::Screen, Uuid::Invalid(), "Home");
    document.CreateInstance(library.Find("Button"), screen);
    const std::string copied = doc::Serializer::ToXml(document);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(copied, loaded, &error, &StandardLibrary()), error);
    CHECK_EQ(loaded.NodeCount(), document.NodeCount());
    CHECK(loaded.Find(library.Find("Button")) != nullptr);
    // And it is the library's Button now, not the file's copy: saving again names it.
    const std::string again = doc::Serializer::ToXml(loaded, true, &StandardLibrary());
    CHECK(again.size() * 10 < copied.size());
}

TEST(library, a_forked_component_beats_the_built_one_in_format_3) {
    doc::Document document;
    const Library library = BuildStandardLibrary(document);
    const Uuid button = library.Find("Button");
    document.Find(button)->name = "Button";
    document.SetProp(button, doc::Prop::CornerRadius, 99.0f);

    const std::string xml = doc::Serializer::ToXml(document, true, &StandardLibrary());
    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(xml, loaded, &error, &StandardLibrary()), error);
    CHECK_EQ(loaded.GetProp(button, doc::Prop::CornerRadius), doc::Value{ 99.0f });
}

TEST(library, the_catalog_comes_back_on_the_same_ids_every_time) {
    // Overrides are keyed by the id of the node INSIDE the component. If a rebuild renumbered
    // them, every instance in every saved file would quietly lose its overrides.
    doc::Document first, second;
    const Library a = BuildStandardLibrary(first);
    const Library b = BuildStandardLibrary(second);
    CHECK(a.Find("Button") == b.Find("Button"));
    CHECK(first.Subtree(a.Find("Button")) == second.Subtree(b.Find("Button")));
    CHECK(a.Find("Button") != a.Find("Card"));
}

TEST(library, a_document_without_the_library_available_says_so) {
    doc::Document document;
    BuildStandardLibrary(document);
    const std::string xml = doc::Serializer::ToXml(document, true, &StandardLibrary(), true);

    doc::Document loaded;
    std::string error;
    CHECK(!doc::Serializer::FromXml(xml, loaded, &error));
    CHECK(error.find("vae.std") != std::string::npos);
}

TEST(library, an_edited_component_is_written_out_and_wins_over_the_built_one) {
    // A designer restyling Button is the case the reference must not swallow: the fork lives in
    // the file that forked it, and reading it back must give the fork, not the stock widget.
    doc::Document document;
    const Library library = BuildStandardLibrary(document);
    const Uuid button = library.Find("Button");
    document.SetProp(button, doc::Prop::CornerRadius, 99.0f);

    const std::string xml = doc::Serializer::ToXml(document, true, &StandardLibrary(), true);
    CHECK(xml.find("library=\"vae.std@") != std::string::npos);

    doc::Document loaded;
    std::string error;
    CHECK_MESSAGE(doc::Serializer::FromXml(xml, loaded, &error, &StandardLibrary()), error);
    CHECK_EQ(loaded.GetProp(button, doc::Prop::CornerRadius), doc::Value{ 99.0f });
    // ...and exactly once: the built copy must not survive beside it.
    CHECK_EQ(loaded.Subtree(button).size(), document.Subtree(button).size());
    CHECK_EQ(std::count(loaded.Roots().begin(), loaded.Roots().end(), button), 1);
}
