#include "Test.h"

#include "vae/a11y/Accessibility.h"
#include "vae/base/FileSystem.h"
#include "vae/doc/Serializer.h"
#include "vae/text/FontDB.h"
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"

#include <string>

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
