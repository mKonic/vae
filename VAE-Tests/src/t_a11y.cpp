#include "Test.h"

#include "vae/a11y/Accessibility.h"
#include "vae/base/FileSystem.h"
#include "vae/doc/Serializer.h"
#include "vae/text/FontDB.h"
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"

#include <string>
#include <vector>

using namespace vae;
using namespace vae::ui;

// What the app looks like to a screen reader, checked without one attached.
//
// This is the half of accessibility that can be wrong silently: a button announced as a panel, a
// label read twice because it is both its own node and part of its button's name, a closed menu
// still in the tree. None of that shows up on screen, and all of it is a unit test.

namespace {

    struct A11yUi {
        doc::Document document;
        Library library;
        UiHost host;
        Uuid screen;
        Vec2 size{ 800.0f, 600.0f };

        A11yUi() {
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

        void Frame(f32 dt = 1.0f / 60.0f) { host.Update(size, dt); }

        // A property set on a placed widget. It lands as an override on the component's root,
        // which is where the editor puts it too — an instance's own props are the component's,
        // and writing straight to the instance node would change every copy.
        void SetOn(Uuid instance, std::string_view widget, doc::Prop prop, doc::Value value) {
            document.SetOverride(instance, library.Find(widget), prop, std::move(value));
            Frame();
        }

        a11y::Tree Build() {
            Frame();
            a11y::Tree tree;
            tree.Build(host.Tree(), "Test app");
            return tree;
        }
    };

    const a11y::Node* FindRole(const a11y::Tree& tree, a11y::Role role) {
        for (const a11y::Node& node : tree.Nodes()) if (node.role == role) return &node;
        return nullptr;
    }

    u32 CountRole(const a11y::Tree& tree, a11y::Role role) {
        u32 count = 0;
        for (const a11y::Node& node : tree.Nodes()) if (node.role == role) ++count;
        return count;
    }

}

// The publisher no longer rebuilds a screen reader's whole tree sixty times a second to find out
// whether anything changed — it asks the host for a stamp first. That is a cache, and the failure a
// cache has is the quiet one: a change nobody is told about leaves a screen reader describing a
// screen that has moved on.
//
// So this is the guard. Every kind of change that reaches the built tree has to move the stamp; a
// frame in which nothing happened must not.
TEST(a11y, a_quiet_frame_does_not_move_the_stamp) {
    A11yUi ui;
    ui.Place("Button");
    for (int i = 0; i < 4; ++i) ui.Frame();

    const u64 settled = ui.host.AccessibilityStamp();
    ui.Frame();
    ui.Frame();
    CHECK_EQ(ui.host.AccessibilityStamp(), settled);
}

TEST(a11y, everything_a_reader_would_notice_moves_the_stamp) {
    A11yUi ui;
    const Uuid button = ui.Place("Button");
    const Uuid box = ui.Place("Checkbox", { 40.0f, 200.0f });
    ui.Frame();

    const auto moved = [&ui](u64 before, const char* what) {
        const u64 now = ui.host.AccessibilityStamp();
        CHECK_MESSAGE(now != before, std::string(what) + " did not move the stamp");
        return now;
    };

    // What it says.
    u64 stamp = ui.host.AccessibilityStamp();
    ui.SetOn(button, "Button", doc::Prop::Text, std::string("Send"));
    stamp = moved(stamp, "changing a label");

    // What state it is in — a checkbox that is now checked is announced differently.
    const u32 view = [&] {
        const ui::ViewTree& tree = ui.host.Tree();
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).instanceId == box && tree.At(i).behavior) return i;
        return ui::ViewTree::kInvalid;
    }();
    CHECK(view != ui::ViewTree::kInvalid);
    ui.host.Tree().SetState(view, ui::StateBit::Checked, true);
    stamp = moved(stamp, "checking a box");

    // Where it is. A screen reader draws a box around what it is reading.
    ui.document.Find(button)->layout.offsetStart = { 300.0f, 300.0f };
    ui.document.Touch(button);
    ui.Frame();
    stamp = moved(stamp, "moving a widget");

    // Whether it is there at all.
    ui.host.Tree().SetRuntimeVisible(view, false);
    stamp = moved(stamp, "hiding a widget");

    // And the caret, which lives on the host rather than in a tree because an edit survives the
    // tree being rebuilt — the one input a tree's own stamp cannot speak for.
    ui.host.EditState(ui::WidgetId{ button }).caret = 3;
    stamp = moved(stamp, "moving the caret");
    ui.host.EditState(ui::WidgetId{ button }).anchor = 1;
    moved(stamp, "extending a selection");
}

TEST(a11y, the_tree_starts_at_a_window) {
    A11yUi ui;
    const a11y::Tree tree = ui.Build();

    CHECK(tree.Count() > 0);
    CHECK_EQ(tree.At(tree.Root()).role, a11y::Role::Window);
    CHECK_EQ(tree.At(tree.Root()).name, std::string("Test app"));
}

TEST(a11y, a_button_is_announced_by_the_text_drawn_on_it) {
    A11yUi ui;
    ui.Place("Button");
    const a11y::Tree tree = ui.Build();

    const a11y::Node* button = FindRole(tree, a11y::Role::PushButton);
    CHECK(button != nullptr);
    if (!button) return;

    // The label inside it IS its name — nobody had to say so.
    CHECK(!button->name.empty());
    CHECK(a11y::Has(button->state, a11y::State::Focusable));
    CHECK(a11y::Has(button->state, a11y::State::Enabled));

    // And that label is not also a node of its own, or a screen reader reads it twice.
    CHECK_EQ(CountRole(tree, a11y::Role::Label), 0u);
}

TEST(a11y, an_explicit_label_wins_over_the_text_inside) {
    A11yUi ui;
    const Uuid button = ui.Place("Button");
    ui.SetOn(button, "Button", doc::Prop::A11yLabel, std::string("Close the dialog"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::PushButton);
    CHECK(node != nullptr);
    if (node) CHECK_EQ(node->name, std::string("Close the dialog"));
}

TEST(a11y, a_checkbox_reports_whether_it_is_checked) {
    A11yUi ui;
    const Uuid box = ui.Place("Checkbox");
    const a11y::Tree unchecked = ui.Build();
    const a11y::Node* before = FindRole(unchecked, a11y::Role::CheckBox);
    CHECK(before != nullptr);
    if (before) CHECK(!a11y::Has(before->state, a11y::State::Checked));

    ui.SetOn(box, "Checkbox", doc::Prop::Checked, true);
    const a11y::Tree checked = ui.Build();
    const a11y::Node* after = FindRole(checked, a11y::Role::CheckBox);
    CHECK(after != nullptr);
    if (after) CHECK(a11y::Has(after->state, a11y::State::Checked));
}

TEST(a11y, a_switch_is_a_toggle_and_not_a_check_box) {
    A11yUi ui;
    ui.Place("Switch");
    const a11y::Tree tree = ui.Build();

    // Pressing a switch turns something on; ticking a check box selects it. A screen reader that
    // says "check box" for a switch is wrong about what pressing it does.
    CHECK(FindRole(tree, a11y::Role::ToggleButton) != nullptr);
    CHECK_EQ(CountRole(tree, a11y::Role::CheckBox), 0u);
}

TEST(a11y, a_slider_carries_its_value_and_its_range) {
    A11yUi ui;
    const Uuid slider = ui.Place("Slider");
    ui.SetOn(slider, "Slider", doc::Prop::Value, 30.0f);
    ui.SetOn(slider, "Slider", doc::Prop::MinValue, 0.0f);
    ui.SetOn(slider, "Slider", doc::Prop::MaxValue, 60.0f);

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::Slider);
    CHECK(node != nullptr);
    if (!node) return;
    CHECK(node->hasValue);
    CHECK_NEAR(node->value, 30.0f);
    CHECK_NEAR(node->maximum, 60.0f);

    // Its track, its fill and its knob are how a slider is drawn, not things to read out.
    CHECK_EQ(CountRole(tree, a11y::Role::Panel), 0u);
}

TEST(a11y, a_text_field_reports_what_is_typed_in_it_separately_from_its_name) {
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Placeholder, std::string("Email"));
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("someone@example.com"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::Entry);
    CHECK(node != nullptr);
    if (!node) return;
    // What it is FOR and what is IN it are different questions with different answers.
    CHECK_EQ(node->name, std::string("Email"));
    CHECK_EQ(node->text, std::string("someone@example.com"));
    CHECK(a11y::Has(node->state, a11y::State::Editable));
}

TEST(a11y, a_password_field_is_not_an_ordinary_entry) {
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Password, true);

    const a11y::Tree tree = ui.Build();
    CHECK(FindRole(tree, a11y::Role::PasswordText) != nullptr);
}

TEST(a11y, a_disabled_control_is_not_focusable) {
    A11yUi ui;
    const Uuid button = ui.Place("Button");
    ui.SetOn(button, "Button", doc::Prop::Enabled, false);

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::PushButton);
    CHECK(node != nullptr);
    if (!node) return;
    CHECK(!a11y::Has(node->state, a11y::State::Enabled));
    CHECK(!a11y::Has(node->state, a11y::State::Focusable));
}

TEST(a11y, a_hidden_subtree_is_not_in_the_tree_at_all) {
    A11yUi ui;
    const Uuid button = ui.Place("Button");
    const a11y::Tree visible = ui.Build();
    CHECK(FindRole(visible, a11y::Role::PushButton) != nullptr);

    ui.SetOn(button, "Button", doc::Prop::Visible, false);
    const a11y::Tree hidden = ui.Build();
    // A screen reader reading out a closed menu is the most confusing thing a bridge can do.
    CHECK(FindRole(hidden, a11y::Role::PushButton) == nullptr);
}

TEST(a11y, a_plain_label_is_its_own_node) {
    A11yUi ui;
    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, ui.screen, "Heading");
    doc::Node* node = ui.document.Find(label);
    node->layout.offsetStart = { 20.0f, 20.0f };
    node->layout.width = layout::Size::Px(200.0f);
    node->layout.height = layout::Size::Px(24.0f);
    ui.document.SetProp(label, doc::Prop::Text, std::string("Settings"));
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    const a11y::Node* found = FindRole(tree, a11y::Role::Label);
    CHECK(found != nullptr);
    if (found) {
        CHECK_EQ(found->name, std::string("Settings"));
        CHECK_EQ(found->text, std::string("Settings"));
        CHECK(!a11y::Has(found->state, a11y::State::Focusable));
    }
}

TEST(a11y, a_control_with_nothing_to_announce_it_is_reported) {
    A11yUi ui;
    // An icon-only button: a box with a role and nothing written on it. Drawn perfectly well,
    // announced as "push button" and nothing else.
    const Uuid button = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "IconButton");
    doc::Node* node = ui.document.Find(button);
    node->layout.offsetStart = { 20.0f, 20.0f };
    node->layout.width = layout::Size::Px(32.0f);
    node->layout.height = layout::Size::Px(32.0f);
    ui.document.SetProp(button, doc::Prop::Role, std::string("button"));
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    bool reported = false;
    for (const auto& problem : tree.Problems())
        if (problem.what.find("announce") != std::string::npos) reported = true;
    CHECK(reported);

    // And naming it is what makes the problem go away.
    ui.document.SetProp(button, doc::Prop::A11yLabel, std::string("Delete"));
    ui.Frame();
    const a11y::Tree named = ui.Build();
    for (const auto& problem : named.Problems())
        CHECK(problem.what.find("announce") == std::string::npos);
}

TEST(a11y, a_control_too_small_to_hit_is_reported) {
    A11yUi ui;
    const Uuid button = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Tiny");
    doc::Node* node = ui.document.Find(button);
    node->layout.offsetStart = { 20.0f, 20.0f };
    node->layout.width = layout::Size::Px(12.0f);
    node->layout.height = layout::Size::Px(12.0f);
    ui.document.SetProp(button, doc::Prop::Role, std::string("button"));
    ui.document.SetProp(button, doc::Prop::A11yLabel, std::string("Delete"));
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    bool reported = false;
    for (const auto& problem : tree.Problems())
        if (problem.what.find("pointer target") != std::string::npos) reported = true;
    CHECK(reported);
}

TEST(a11y, every_widget_in_the_library_has_a_role_that_is_not_a_panel) {
    // A widget that falls through the role mapping is announced as an unlabelled group, which is
    // the failure mode nobody notices until somebody tries to use the app without a screen.
    struct Case { const char* widget; a11y::Role role; };
    const Case cases[] = {
        { "Button", a11y::Role::PushButton }, { "Checkbox", a11y::Role::CheckBox },
        { "Radio", a11y::Role::RadioButton }, { "Switch", a11y::Role::ToggleButton },
        { "Slider", a11y::Role::Slider },     { "Dropdown", a11y::Role::ComboBox },
        { "TextInput", a11y::Role::Entry },   { "Tabs", a11y::Role::PageTabList },
        { "Progress", a11y::Role::ProgressBar },
    };

    for (const Case& c : cases) {
        A11yUi ui;
        ui.Place(c.widget);
        const a11y::Tree tree = ui.Build();
        const bool found = FindRole(tree, c.role) != nullptr;
        if (!found) VAE_ERROR("a11y: {} is not announced as {}", c.widget, a11y::RoleName(c.role));
        CHECK(found);
    }
}

// --- acting, not just reading -------------------------------------------------------------------
//
// The tree above is what a screen reader hears. These are what it can *do* — the actions it offers
// the user, and the caret it needs to follow a typist through a field. Both are computed here and
// carried by the AT-SPI bridge, which is the half that needs a bus and cannot be a unit test.

namespace {

    std::vector<std::string> ActionNames(const a11y::Node& node) {
        std::vector<std::string> out;
        for (u32 i = 0; i < static_cast<u32>(a11y::Action::Count); ++i)
            if (a11y::Has(node.actions, static_cast<a11y::Action>(i)))
                out.emplace_back(a11y::ActionName(static_cast<a11y::Action>(i)));
        return out;
    }

}

TEST(a11y, a_button_offers_exactly_one_thing_to_do) {
    A11yUi ui;
    ui.Place("Button");
    const a11y::Tree tree = ui.Build();

    const a11y::Node* button = FindRole(tree, a11y::Role::PushButton);
    CHECK(button != nullptr);
    if (!button) return;
    CHECK(ActionNames(*button) == std::vector<std::string>{ "click" });
}

TEST(a11y, a_checkbox_says_it_toggles_rather_than_only_that_it_clicks) {
    A11yUi ui;
    ui.Place("Checkbox");
    const a11y::Tree tree = ui.Build();

    const a11y::Node* box = FindRole(tree, a11y::Role::CheckBox);
    CHECK(box != nullptr);
    if (!box) return;
    // Both, and in this order: a screen reader reads the first out as the default and the second
    // is what actually describes the effect.
    CHECK(ActionNames(*box) == (std::vector<std::string>{ "click", "toggle" }));
}

TEST(a11y, a_dropdown_offers_to_expand_and_then_to_collapse) {
    A11yUi ui;
    const Uuid drop = ui.Place("Dropdown");

    const a11y::Tree closed = ui.Build();
    const a11y::Node* shut = FindRole(closed, a11y::Role::ComboBox);
    CHECK(shut != nullptr);
    if (!shut) return;
    CHECK(a11y::Has(shut->state, a11y::State::Expandable));
    CHECK(!a11y::Has(shut->state, a11y::State::Expanded));
    CHECK(a11y::Has(shut->actions, a11y::Action::Expand));
    CHECK(!a11y::Has(shut->actions, a11y::Action::Collapse));

    // Opened by clicking it, which is the same gesture the bridge performs.
    const Rect bounds = closed.At(closed.ForView(shut->view)).bounds;
    const Vec2 centre = bounds.Center();
    ui.host.DispatchAll({ MakeMouseMoved(centre.x, centre.y),
                          MakeMouseButton(EventType::MouseButtonPressed, Mouse::Left,
                                          centre.x, centre.y, Mod::None),
                          MakeMouseButton(EventType::MouseButtonReleased, Mouse::Left,
                                          centre.x, centre.y, Mod::None) });
    (void)drop;

    const a11y::Tree open = ui.Build();
    const a11y::Node* shown = FindRole(open, a11y::Role::ComboBox);
    CHECK(shown != nullptr);
    if (!shown) return;
    CHECK(a11y::Has(shown->state, a11y::State::Expanded));
    // One or the other, never both: offering to open what is open is an instruction that does
    // nothing, which is worse than not offering it.
    CHECK(a11y::Has(shown->actions, a11y::Action::Collapse));
    CHECK(!a11y::Has(shown->actions, a11y::Action::Expand));
}

TEST(a11y, a_switch_is_not_expandable_and_does_not_offer_to_open) {
    // A switch and a collapsible section are both announced as toggle buttons, and only one of
    // them contains anything. Deciding on the announced role told a screen reader it could open
    // a switch.
    A11yUi ui;
    ui.Place("Switch");
    const a11y::Tree tree = ui.Build();

    const a11y::Node* toggle = FindRole(tree, a11y::Role::ToggleButton);
    CHECK(toggle != nullptr);
    if (!toggle) return;
    CHECK(!a11y::Has(toggle->state, a11y::State::Expandable));
    CHECK(ActionNames(*toggle) == (std::vector<std::string>{ "click", "toggle" }));
}

TEST(a11y, a_disabled_control_offers_nothing_to_do) {
    A11yUi ui;
    const Uuid button = ui.Place("Button");
    ui.SetOn(button, "Button", doc::Prop::Enabled, false);

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::PushButton);
    CHECK(node != nullptr);
    if (node) CHECK_EQ(node->actions, 0u);
}

TEST(a11y, a_label_is_read_and_never_operated) {
    A11yUi ui;
    const Uuid heading = ui.document.CreateNode(doc::NodeKind::Text, ui.screen, "Heading");
    ui.document.Find(heading)->props.Set(doc::Prop::Text, std::string("A heading"));
    ui.document.Touch(heading);
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    const a11y::Node* label = FindRole(tree, a11y::Role::Label);
    CHECK(label != nullptr);
    if (label) CHECK_EQ(label->actions, 0u);
}

TEST(a11y, an_entry_reports_where_the_caret_is_in_characters) {
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    // Four characters, seven bytes: AT-SPI counts characters and an edit state counts bytes, and
    // a field with anything but ASCII in it is where the two stop agreeing.
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("na\xC3\xAF" "ve"));
    ui.Frame();

    a11y::Tree tree;
    // Caret after the ï, which is byte 4 and character 3.
    tree.Build(ui.host.Tree(), "Test app", [](u32, u32& caret, u32& anchor) {
        caret = anchor = 4;
        return true;
    });

    const a11y::Node* entry = FindRole(tree, a11y::Role::Entry);
    CHECK(entry != nullptr);
    if (!entry) return;
    CHECK(entry->hasCaret);
    CHECK_EQ(entry->caret, 3u);
    CHECK_EQ(entry->selectionStart, 3u);
    CHECK_EQ(entry->selectionEnd, 3u);
    CHECK_EQ(entry->text, std::string("na\xC3\xAF" "ve"));
}

TEST(a11y, a_selection_is_reported_lowest_offset_first) {
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("hello world"));
    ui.Frame();

    a11y::Tree tree;
    // Dragged backwards: the caret is before the anchor, and AT-SPI wants a range.
    tree.Build(ui.host.Tree(), "Test app", [](u32, u32& caret, u32& anchor) {
        caret = 2; anchor = 8;
        return true;
    });

    const a11y::Node* entry = FindRole(tree, a11y::Role::Entry);
    CHECK(entry != nullptr);
    if (!entry) return;
    CHECK_EQ(entry->caret, 2u);
    CHECK_EQ(entry->selectionStart, 2u);
    CHECK_EQ(entry->selectionEnd, 8u);
}

TEST(a11y, no_caret_source_means_no_caret_rather_than_offset_zero) {
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("hello"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* entry = FindRole(tree, a11y::Role::Entry);
    CHECK(entry != nullptr);
    if (entry) CHECK(!entry->hasCaret);
}

TEST(a11y, a_password_field_reports_its_length_and_never_its_contents) {
    // The tree is published on a bus any process on the session can read, and the Text interface
    // hands out exactly this string. "What is in that field" is the question a password field
    // exists to refuse.
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Password, true);
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("hunter2"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* secret = FindRole(tree, a11y::Role::PasswordText);
    CHECK(secret != nullptr);
    if (!secret) return;
    CHECK_EQ(secret->text, std::string("*******"));
    CHECK(secret->text.find("hunter") == std::string::npos);
    CHECK(secret->name.find("hunter") == std::string::npos);
}

TEST(a11y, the_same_control_keeps_its_key_across_a_rebuild) {
    // What lets the bridge say "that checkbox is now checked" instead of "the screen is new".
    // Tree position is not identity: the view tree is rebuilt from scratch every frame.
    A11yUi ui;
    const Uuid box = ui.Place("Checkbox");

    const a11y::Tree before = ui.Build();
    const a11y::Node* first = FindRole(before, a11y::Role::CheckBox);
    CHECK(first != nullptr);
    if (!first) return;
    const u64 key = first->key;
    CHECK(key != 0u);

    // Something else appears above it, so every index below moves.
    ui.Place("Button", { 40.0f, 300.0f });
    ui.SetOn(box, "Checkbox", doc::Prop::Checked, true);

    const a11y::Tree after = ui.Build();
    const a11y::Node* second = FindRole(after, a11y::Role::CheckBox);
    CHECK(second != nullptr);
    if (!second) return;
    CHECK_EQ(second->key, key);
    CHECK(a11y::Has(second->state, a11y::State::Checked));

    // And two different controls are not the same thing.
    const a11y::Node* button = FindRole(after, a11y::Role::PushButton);
    CHECK(button != nullptr);
    if (button) CHECK(button->key != key);
}

TEST(a11y, moving_the_caret_changes_the_signature_so_it_is_published) {
    // Publish only happens when the signature changes, so a caret left out of it is a caret a
    // screen reader never hears move — which is the whole of following a typist.
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("hello"));
    ui.Frame();

    const auto Signature = [&ui](u32 at) {
        a11y::Tree tree;
        tree.Build(ui.host.Tree(), "Test app", [at](u32, u32& caret, u32& anchor) {
            caret = anchor = at;
            return true;
        });
        return tree.Signature();
    };
    CHECK(Signature(1) != Signature(4));
    CHECK_EQ(Signature(2), Signature(2));
}

TEST(a11y, an_entry_reports_where_each_character_is_drawn) {
    // A screen reader draws a box around what it is reading and a magnifier follows it. The
    // answer has to come from the run that was shaped: dividing the field's box by the number
    // of characters in it is a whole character out within a few words of proportional type.
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("Hello"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::Entry);
    CHECK(node != nullptr);
    if (!node) return;

    CHECK_EQ(node->characters.size(), std::size_t{ 5 });
    if (node->characters.size() != 5) return;
    for (std::size_t i = 0; i < node->characters.size(); ++i) {
        const Rect& box = node->characters[i];
        CHECK(box.size.x > 0.0f);
        CHECK(box.size.y > 0.0f);
        // Inside the field it is drawn in, and after the character before it.
        CHECK(box.pos.x >= node->bounds.pos.x);
        CHECK(box.pos.x + box.size.x <= node->bounds.pos.x + node->bounds.size.x + 1.0f);
        if (i > 0) CHECK(box.pos.x > node->characters[i - 1].pos.x);
    }
}

TEST(a11y, a_wrapped_label_puts_its_second_line_below_the_first) {
    // The proof that these come from the shaped run rather than from arithmetic on the box: an
    // even split has one row of boxes however the text was drawn, and a wrapped label has two.
    A11yUi ui;
    const Uuid label = ui.document.CreateNode(doc::NodeKind::Text, ui.screen, "Paragraph");
    doc::Node* node = ui.document.Find(label);
    node->layout.offsetStart = { 20.0f, 20.0f };
    node->layout.width = layout::Size::Px(90.0f);
    node->layout.height = layout::Size::Px(80.0f);
    ui.document.SetProp(label, doc::Prop::Text, std::string("wrap me over lines"));
    ui.document.SetProp(label, doc::Prop::TextWrap, std::string("word"));
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    const a11y::Node* found = FindRole(tree, a11y::Role::Label);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK_EQ(found->characters.size(), std::size_t{ 18 });
    if (found->characters.empty()) return;

    // Somewhere in there the text goes back to the left and down a line.
    bool wrapped = false;
    for (std::size_t i = 1; i < found->characters.size(); ++i)
        if (found->characters[i].pos.y > found->characters[i - 1].pos.y
            && found->characters[i].pos.x < found->characters[i - 1].pos.x)
            wrapped = true;
    CHECK(wrapped);
}

TEST(a11y, a_field_showing_its_placeholder_reports_no_character_boxes) {
    // The placeholder is drawn and the field's text is empty, so a box per character would be a
    // box per character of a string nobody is being read. Nothing is the honest answer, and the
    // bridge falls back to the even split for it.
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Placeholder, std::string("Email"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::Entry);
    CHECK(node != nullptr);
    if (node) CHECK(node->characters.empty());
}

TEST(a11y, a_password_field_reports_a_box_per_bullet_and_not_its_contents) {
    A11yUi ui;
    const Uuid field = ui.Place("TextInput");
    ui.SetOn(field, "TextInput", doc::Prop::Password, true);
    ui.SetOn(field, "TextInput", doc::Prop::Text, std::string("hunter2"));

    const a11y::Tree tree = ui.Build();
    const a11y::Node* node = FindRole(tree, a11y::Role::PasswordText);
    CHECK(node != nullptr);
    if (!node) return;
    // As many boxes as there are characters, because that is what is on the screen — and the
    // text beside them is still asterisks rather than the password.
    CHECK_EQ(node->text, std::string("*******"));
    CHECK_EQ(node->characters.size(), std::size_t{ 7 });
}

TEST(a11y, a_repeated_container_is_a_list_and_its_copies_are_items) {
    // Every list in VAE is a repeated container — there is no list widget. Without this a screen
    // reader is handed a stack of frames and cannot say "item 3 of 4", and the Selection
    // interface on the bus has nothing to choose between.
    A11yUi ui;
    const Uuid list = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Channels");
    {
        doc::Node* node = ui.document.Find(list);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(240.0f);
        node->layout.height = layout::Size::Px(200.0f);
    }
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, list, "Row");
    {
        doc::Node* node = ui.document.Find(row);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(32.0f);
    }
    const Uuid title = ui.document.CreateNode(doc::NodeKind::Text, row, "Title");
    ui.document.SetProp(title, doc::Prop::Field, std::string("name"));
    ui.Frame();

    doc::RowTable rows;
    rows.columns = { "name" };
    rows.cells = { "general", "design", "random" };
    ui.host.Tree().SetRows(WidgetId{ list, Uuid::Invalid() }, rows);
    ui.host.MarkDirty();
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    CHECK_EQ(CountRole(tree, a11y::Role::List), 1u);
    CHECK_EQ(CountRole(tree, a11y::Role::ListItem), 3u);

    // Each item is announced by what its row drew, and every one of them can be chosen.
    std::vector<std::string> names;
    for (const a11y::Node& node : tree.Nodes())
        if (node.role == a11y::Role::ListItem) {
            names.push_back(node.name);
            CHECK(a11y::Has(node.state, a11y::State::Selectable));
        }
    CHECK_EQ(names, (std::vector<std::string>{ "general", "design", "random" }));
}

TEST(a11y, the_row_a_list_marks_selected_is_the_one_reported_selected) {
    A11yUi ui;
    const Uuid list = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Channels");
    {
        doc::Node* node = ui.document.Find(list);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Column;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(240.0f);
        node->layout.height = layout::Size::Px(200.0f);
    }
    ui.document.SetProp(list, doc::Prop::SelectedIndex, 1.0f);
    const Uuid row = ui.document.CreateNode(doc::NodeKind::Frame, list, "Row");
    {
        doc::Node* node = ui.document.Find(row);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.width = layout::Size::Fill();
        node->layout.height = layout::Size::Px(32.0f);
    }
    const Uuid title = ui.document.CreateNode(doc::NodeKind::Text, row, "Title");
    ui.document.SetProp(title, doc::Prop::Field, std::string("name"));
    ui.Frame();

    doc::RowTable rows;
    rows.columns = { "name" };
    rows.cells = { "general", "design", "random" };
    ui.host.Tree().SetRows(WidgetId{ list, Uuid::Invalid() }, rows);
    ui.host.MarkDirty();
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    u32 selected = 0;
    std::string chosen;
    for (const a11y::Node& node : tree.Nodes())
        if (node.role == a11y::Role::ListItem && a11y::Has(node.state, a11y::State::Selected)) {
            ++selected;
            chosen = node.name;
        }
    CHECK_EQ(selected, 1u);
    CHECK_EQ(chosen, std::string("design"));
}

TEST(a11y, a_repeated_row_with_nothing_to_say_is_not_announced_as_an_item) {
    // A strip of decoration — dots under a carousel, bars in a chart — is repeated too, and
    // announcing four unnamed list items is four things the user has to walk past to get on.
    A11yUi ui;
    const Uuid strip = ui.document.CreateNode(doc::NodeKind::Frame, ui.screen, "Dots");
    {
        doc::Node* node = ui.document.Find(strip);
        node->layout.mode = layout::LayoutMode::Stack;
        node->layout.axis = layout::Axis::Row;
        node->layout.offsetStart = { 20.0f, 20.0f };
        node->layout.width = layout::Size::Px(240.0f);
        node->layout.height = layout::Size::Px(20.0f);
    }
    const Uuid dot = ui.document.CreateNode(doc::NodeKind::Frame, strip, "Dot");
    {
        doc::Node* node = ui.document.Find(dot);
        node->layout.width = layout::Size::Px(8.0f);
        node->layout.height = layout::Size::Px(8.0f);
    }
    ui.document.SetProp(strip, doc::Prop::Repeat, 4.0f);
    ui.Frame();

    const a11y::Tree tree = ui.Build();
    CHECK_EQ(CountRole(tree, a11y::Role::ListItem), 0u);
    // And the strip around them is not a list either: a list of nothing is still a stop.
    CHECK_EQ(CountRole(tree, a11y::Role::List), 0u);
    for (const a11y::Tree::Problem& problem : tree.Problems())
        CHECK(problem.what.find("list item") == std::string::npos);
}
