#include "vaepch.h"
#include "vae/a11y/Accessibility.h"

#include "vae/ui/ViewTree.h"

#include <array>

namespace vae::a11y {

    namespace {
        constexpr std::array<const char*, static_cast<std::size_t>(Role::Count)> kRoleNames{
            "invalid",
            "application", "window", "dialog", "alert", "panel", "filler",
            "push button", "toggle button", "check box", "radio button", "slider", "combo box",
            "entry", "password text",
            "page tab list", "page tab", "scroll pane", "list", "list item", "table",
            "column header",
            "menu", "menu item", "tool tip", "notification", "progress bar", "separator", "label",
            "image", "link", "canvas", "date editor",
        };
    }

    const char* RoleName(Role role) {
        const auto index = static_cast<std::size_t>(role);
        return index < kRoleNames.size() ? kRoleNames[index] : "invalid";
    }

    // VAE's roles are about behaviour and AT-SPI's are about what a thing is called. Mostly they
    // agree; the disagreements are the interesting ones and are commented where they happen.
    Role RoleFor(ui::Role role, bool multiline, bool password) {
        using ui::Role;
        switch (role) {
            case Role::Button:       return a11y::Role::PushButton;
            case Role::TextInput:    return password ? a11y::Role::PasswordText : a11y::Role::Entry;
            case Role::Checkbox:     return a11y::Role::CheckBox;
            case Role::Radio:        return a11y::Role::RadioButton;
            // A switch is a toggle button, not a check box: it is on or off rather than ticked,
            // and a screen reader announcing "check box, checked" for one is wrong about what
            // pressing it does.
            case Role::Switch:       return a11y::Role::ToggleButton;
            case Role::Slider:       return a11y::Role::Slider;
            case Role::Dropdown:
            case Role::Combobox:     return a11y::Role::ComboBox;
            case Role::DropdownItem: return a11y::Role::MenuItem;
            case Role::Tabs:         return a11y::Role::PageTabList;
            case Role::Tab:          return a11y::Role::PageTab;
            case Role::Scroll:       return a11y::Role::ScrollPane;
            case Role::List:
            case Role::Carousel:     return a11y::Role::List;
            case Role::ListItem:     return a11y::Role::ListItem;
            case Role::Table:        return a11y::Role::Table;
            case Role::TableColumn:  return a11y::Role::ColumnHeader;
            case Role::Modal:        return a11y::Role::Dialog;
            case Role::Popover:      return a11y::Role::Dialog;
            case Role::Toast:        return a11y::Role::Notification;
            case Role::Tooltip:      return a11y::Role::ToolTip;
            case Role::Menu:
            case Role::ContextMenu:  return a11y::Role::Menu;
            case Role::Progress:     return a11y::Role::ProgressBar;
            case Role::Splitter:     return a11y::Role::Separator;
            case Role::Collapsible:  return a11y::Role::ToggleButton;
            case Role::Calendar:     return a11y::Role::DateEditor;
            case Role::Chart:        return a11y::Role::Canvas;
            case Role::InputOtp:     return a11y::Role::Entry;
            case Role::Pagination:   return a11y::Role::Panel;
            // A router is where a screen goes; it is not a thing on screen.
            case Role::Router:
            case Role::Accordion:    return a11y::Role::Panel;
            // The parts a behaviour owns. A slider's knob is not separately announced — the slider
            // is the control, and reading its pieces out is noise between the user and the value.
            case Role::Track: case Role::Fill: case Role::Knob: case Role::Indicator:
            case Role::Thumb: case Role::Content: case Role::Anchor: case Role::Scrim:
                return a11y::Role::Invalid;
            case Role::None:
            default:
                break;
        }
        (void)multiline;
        return a11y::Role::Invalid;
    }

    namespace {

        // Can this be operated, and therefore does it need a name?
        bool Interactive(Role role) {
            switch (role) {
                case Role::PushButton: case Role::ToggleButton: case Role::CheckBox:
                case Role::RadioButton: case Role::Slider: case Role::ComboBox:
                case Role::Entry: case Role::PasswordText: case Role::PageTab:
                case Role::MenuItem: case Role::ListItem: case Role::Link:
                    return true;
                default:
                    return false;
            }
        }

        bool ReadsAValue(Role role) {
            return role == Role::Slider || role == Role::ProgressBar;
        }

        StateSet StatesOf(const ui::ViewTree& views, u32 view, Role role) {
            const ui::ViewTree::View& v = views.At(view);
            StateSet set = 0;
            if (v.visible) set = set | State::Visible | State::Showing;

            const bool disabled = ui::HasState(v.state, ui::StateBit::Disabled)
                               || !views.Flag(view, doc::Prop::Enabled, true);
            if (!disabled) set = set | State::Enabled | State::Sensitive;

            if (Interactive(role) && !disabled) set = set | State::Focusable;
            if (ui::HasState(v.state, ui::StateBit::Focused))  set = set | State::Focused;
            if (ui::HasState(v.state, ui::StateBit::Checked))  set = set | State::Checked;
            if (ui::HasState(v.state, ui::StateBit::Pressed))  set = set | State::Pressed;
            if (ui::HasState(v.state, ui::StateBit::Selected)) set = set | State::Selected;

            if (role == Role::ListItem || role == Role::PageTab || role == Role::MenuItem)
                set = set | State::Selectable;
            if (role == Role::Entry || role == Role::PasswordText)
                if (!views.Flag(view, doc::Prop::ReadOnly, false)) set = set | State::Editable;
            if (role == Role::ComboBox || role == Role::ToggleButton) {
                set = set | State::Expandable;
                if (ui::HasState(v.state, ui::StateBit::Open)) set = set | State::Expanded;
            }
            if (role == Role::Slider || role == Role::ProgressBar) set = set | State::Horizontal;
            return set;
        }

        // The text a node contributes to the name of whatever contains it. A button's name is the
        // label drawn on it, so the label's text has to be reachable from the button — and only
        // from the button, because a label that is announced on its own AND as part of its button
        // is read twice.
        void GatherText(const ui::ViewTree& views, u32 view, std::string& out, u32 depth = 0) {
            if (depth > 8 || !views.Valid(view)) return;
            const ui::ViewTree::View& v = views.At(view);
            if (!v.visible) return;

            if (v.kind == doc::NodeKind::Text) {
                const std::string text = views.Str(view, doc::Prop::Text);
                if (!text.empty()) {
                    if (!out.empty()) out += ' ';
                    out += text;
                }
            }
            for (u32 child : v.children) GatherText(views, child, out, depth + 1);
        }

    }

    void Tree::Clear() {
        m_Nodes.clear();
        m_Problems.clear();
        m_ByView.clear();
        m_Signature = 0;
    }

    u32 Tree::Add(u32 parent, Node node) {
        const auto index = static_cast<u32>(m_Nodes.size());
        node.parent = parent;
        m_Nodes.push_back(std::move(node));
        if (parent != Node::kInvalid) m_Nodes[parent].children.push_back(index);
        return index;
    }

    u32 Tree::ForView(u32 view) const {
        return view < m_ByView.size() ? m_ByView[view] : Node::kInvalid;
    }

    u32 Tree::Focused() const {
        for (u32 i = 0; i < m_Nodes.size(); ++i)
            if (Has(m_Nodes[i].state, State::Focused)) return i;
        return Node::kInvalid;
    }

    void Tree::Walk(const ui::ViewTree& views, u32 view, u32 parent) {
        if (!views.Valid(view)) return;
        const ui::ViewTree::View& v = views.At(view);
        // An invisible subtree is not announced at all. A screen reader reading out a closed menu
        // is the single most confusing thing a bridge can do.
        if (!v.visible) return;

        const bool password = views.Flag(view, doc::Prop::Password, false);
        const bool multiline = views.Flag(view, doc::Prop::Multiline, false);
        Role role = RoleFor(v.role, multiline, password);

        // A text node with no role of its own is a label — unless something above it already
        // speaks for it, in which case it is part of that thing's name and not a node.
        bool isLabel = false;
        if (role == Role::Invalid && v.kind == doc::NodeKind::Text) {
            role = Role::Label;
            isLabel = true;
        }

        if (role == Role::Invalid) {
            // Nothing to announce here; its children may still have something.
            for (u32 child : v.children) Walk(views, child, parent);
            return;
        }

        Node node;
        node.role = role;
        node.view = view;
        node.bounds = views.Bounds(view);
        node.state = StatesOf(views, view, role);

        node.description = views.Str(view, doc::Prop::Tooltip);
        node.name = views.Str(view, doc::Prop::A11yLabel);
        if (node.name.empty()) {
            const bool entry = role == Role::Entry || role == Role::PasswordText;
            if (isLabel) {
                node.name = views.Str(view, doc::Prop::Text);
                node.text = node.name;
            } else if (entry) {
                // Deliberately not the text drawn inside it. What a field displays is what has
                // been typed into it, and announcing a field as "someone@example.com" instead of
                // as "Email" tells the user what they already know and not what it is for.
            } else {
                // The text drawn inside it. This is what makes "Save" the name of the button with
                // Save written on it without anybody having to say so.
                GatherText(views, view, node.name);
            }
        }
        if (node.name.empty() && (role == Role::Entry || role == Role::PasswordText))
            node.name = views.Str(view, doc::Prop::Placeholder);
        // Deliberately no fall back to the node's name. That is what the designer called the layer
        // — "Frame 12", "IconButton" — and announcing it would hide exactly the case this is meant
        // to catch behind a string that means nothing to whoever is listening.

        if (role == Role::Entry || role == Role::PasswordText)
            node.text = views.Str(view, doc::Prop::Text);

        if (ReadsAValue(role)) {
            node.hasValue = true;
            node.value   = views.Number(view, doc::Prop::Value, 0.0f);
            node.minimum = views.Number(view, doc::Prop::MinValue, 0.0f);
            node.maximum = views.Number(view, doc::Prop::MaxValue, 100.0f);
            node.step    = views.Number(view, doc::Prop::Step, 1.0f);
        }

        const u32 index = Add(parent, std::move(node));
        if (view < m_ByView.size()) m_ByView[view] = index;

        const Node& added = m_Nodes[index];
        if (Interactive(added.role) && added.name.empty())
            m_Problems.push_back({ index, std::string(RoleName(added.role))
                                          + " has nothing to announce it by" });
        // 24px is the smallest a pointer target is usually allowed to be, and a control below it is
        // hard to hit for exactly the people this whole tree is for.
        if (Interactive(added.role) && !added.bounds.Empty()
            && (added.bounds.size.x < 24.0f || added.bounds.size.y < 24.0f))
            m_Problems.push_back({ index, std::string(RoleName(added.role))
                                          + " is smaller than a pointer target" });

        // A label that was folded into its container's name is not also a node. Anything else
        // inside a control still is — a list holds its items.
        const bool spokenFor = Interactive(added.role) || added.role == Role::Label;
        for (u32 child : views.At(view).children) {
            if (spokenFor && views.At(child).kind == doc::NodeKind::Text
                          && RoleFor(views.At(child).role, false, false) == Role::Invalid)
                continue;
            Walk(views, child, index);
        }
    }

    void Tree::Build(const ui::ViewTree& views, std::string_view windowName) {
        Clear();
        m_ByView.assign(views.ViewCount(), Node::kInvalid);

        Node window;
        window.role = Role::Window;
        window.name = std::string(windowName);
        window.state = State::Enabled | State::Sensitive | State::Visible | State::Showing;
        if (views.Valid(views.Root())) window.bounds = views.Bounds(views.Root());
        Add(Node::kInvalid, std::move(window));

        if (views.Valid(views.Root())) Walk(views, views.Root(), 0);

        // Everything that would change what is announced. Position is in it because a screen
        // reader draws a box around what it is reading; the sub-pixel part is not, because a
        // one-frame drift in a layout animation is not news.
        const auto mix = [this](u64 value) {
            m_Signature = (m_Signature ^ value) * 0x100000001B3ull;
        };
        for (const Node& node : m_Nodes) {
            mix(static_cast<u64>(node.role));
            mix(node.state);
            mix(std::hash<std::string>{}(node.name));
            mix(std::hash<std::string>{}(node.text));
            mix(static_cast<u64>(static_cast<i64>(node.bounds.pos.x)));
            mix(static_cast<u64>(static_cast<i64>(node.bounds.pos.y)));
            mix(static_cast<u64>(static_cast<i64>(node.bounds.size.x)));
            mix(static_cast<u64>(static_cast<i64>(node.bounds.size.y)));
            if (node.hasValue) mix(static_cast<u64>(static_cast<i64>(node.value * 1000.0f)));
            mix(node.children.size());
        }
    }

}
