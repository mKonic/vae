#pragma once

#include "vae/base/Math.h"
#include "vae/ui/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace vae::ui { class ViewTree; }

namespace vae::a11y {

    // What an app looks like to a screen reader.
    //
    // A separate tree, not a flag on the view tree, because the two disagree about what a node is.
    // A button drawn as a frame with a text inside it is one thing to a screen reader and three
    // views to the renderer; a rounded rectangle that exists to draw a shadow is a view and is
    // nothing at all here. Building it separately is also what makes it testable without a screen
    // reader attached, which is the only way this stays correct.
    //
    // The vocabulary is AT-SPI's rather than VAE's, because that is what the thing on the other end
    // speaks. Translating at the boundary keeps the widget roles free to be about behaviour.
    enum class Role : u16 {
        Invalid = 0,
        Application, Window, Dialog, Alert, Panel, Filler,
        PushButton, ToggleButton, CheckBox, RadioButton, Slider, ComboBox, Entry, PasswordText,
        PageTabList, PageTab, ScrollPane, List, ListItem, Table, ColumnHeader,
        Menu, MenuItem, ToolTip, Notification, ProgressBar, Separator, Label, Image, Link,
        Canvas, DateEditor, Count
    };

    const char* RoleName(Role role);

    // The AT-SPI state bits an app of this shape can actually be in. Not the full set — that has
    // forty-odd members, most of which describe things VAE has no notion of.
    enum class State : u32 {
        Enabled     = 1u << 0,
        Sensitive   = 1u << 1,   // can be interacted with; the opposite of disabled
        Visible     = 1u << 2,
        Showing     = 1u << 3,   // visible AND on screen, which is not the same thing
        Focusable   = 1u << 4,
        Focused     = 1u << 5,
        Checked     = 1u << 6,
        Selected    = 1u << 7,
        Selectable  = 1u << 8,
        Editable    = 1u << 9,
        Expanded    = 1u << 10,
        Expandable  = 1u << 11,
        Pressed     = 1u << 12,
        Horizontal  = 1u << 13,
        Vertical    = 1u << 14,
    };

    using StateSet = u32;
    inline StateSet operator|(State a, State b) { return static_cast<u32>(a) | static_cast<u32>(b); }
    inline StateSet operator|(StateSet a, State b) { return a | static_cast<u32>(b); }
    inline bool Has(StateSet set, State bit) { return (set & static_cast<u32>(bit)) != 0; }

    // What can be *done* to a node, as opposed to read off it. A screen reader offers these to the
    // user by name and then asks the app to perform one, which is the whole difference between an
    // app somebody can hear and an app somebody can use. Deliberately a short list: these are the
    // things a VAE control really has, not a translation of every event a widget can receive.
    enum class Action : u8 { Click, Toggle, Expand, Collapse, Count };

    const char* ActionName(Action action);
    const char* ActionDescription(Action action);

    using ActionSet = u32;
    inline ActionSet Only(Action action) { return 1u << static_cast<u32>(action); }
    inline bool Has(ActionSet set, Action action) { return (set & Only(action)) != 0; }

    struct Node {
        static constexpr u32 kInvalid = 0xFFFFFFFFu;

        u32 parent = kInvalid;
        std::vector<u32> children;

        Role role = Role::Panel;
        // What a screen reader says. Never empty for anything interactive — a button nobody named
        // is announced by the text inside it, and one with neither is a bug worth being able to
        // find, which is what Problems() is for.
        std::string name;
        std::string description;
        // The text a text node actually holds, which is not the same as its name: a label's name
        // IS its text, but an entry's name is what it is for and its text is what is typed in it.
        std::string text;
        StateSet state = 0;
        Rect bounds{ { 0.0f, 0.0f }, { 0.0f, 0.0f } };

        bool hasValue = false;
        f32 value = 0.0f, minimum = 0.0f, maximum = 100.0f, step = 1.0f;

        // What a screen reader may ask to have done to this. Empty for anything that is only read.
        ActionSet actions = 0;

        // Where the caret is in `text` and what of it is selected, in **characters**. AT-SPI counts
        // characters and VAE stores byte offsets, and a field with anything but ASCII in it reports
        // the wrong place in itself if that conversion is skipped. Only an entry has these.
        bool hasCaret = false;
        u32 caret = 0;
        u32 selectionStart = 0, selectionEnd = 0;

        // Where each character of `text` is on screen, in the same space as `bounds`, one rect per
        // character. A screen reader draws a box around what it is reading and magnifiers follow
        // it, so the answer has to come from the run that was actually shaped rather than from
        // dividing the field's box by the number of characters in it — proportional type makes
        // that wrong by a whole character within a few words. Empty when the text was not laid
        // out, which is the signal to fall back rather than to report a box of nothing.
        std::vector<Rect> characters;

        // Back to the view this came from, so a screen reader's "press this" reaches the widget.
        u32 view = kInvalid;

        // What this node *is*, independently of where it landed in either tree. Two builds of the
        // same screen give the same key for the same control, which is what lets the bridge tell a
        // node that changed from one that merely moved — and so say "checked" to a screen reader
        // instead of "the whole screen is new". Zero for the window, which is always node 0.
        u64 key = 0;
    };

    // The tree, rebuilt from a view tree whenever the view tree changes.
    class Tree {
    public:
        // Where the caret is in a text field, in bytes into its text, and where its selection was
        // anchored. A parameter because the view tree does not hold it: an edit state has to
        // survive the tree being rebuilt, so it lives on the host instead. Absent is fine — a
        // document being inspected rather than run has no carets in it.
        using CaretSource = std::function<bool(u32 view, u32& caret, u32& anchor)>;

        void Build(const ui::ViewTree& views, std::string_view windowName,
                   const CaretSource& carets = {});
        void Clear();

        const std::vector<Node>& Nodes() const { return m_Nodes; }
        const Node& At(u32 index) const { return m_Nodes[index]; }
        std::size_t Count() const { return m_Nodes.size(); }
        u32 Root() const { return m_Nodes.empty() ? Node::kInvalid : 0; }

        // The node for a view, or kInvalid when that view was pruned.
        u32 ForView(u32 view) const;
        // The focused node, or kInvalid.
        u32 Focused() const;

        // Everything a screen reader would stumble on: an interactive node with no name, a control
        // smaller than a pointer can reasonably hit. Not an error — an app is allowed to ship with
        // them — but they are the list a designer needs to see.
        struct Problem {
            u32 node = Node::kInvalid;
            std::string what;
        };
        const std::vector<Problem>& Problems() const { return m_Problems; }

        // Everything a screen reader would notice, in one number. Rebuilding the tree is cheap;
        // telling a screen reader that the whole screen is new is not — it reads the screen out
        // again — so a frame that changed nothing has to be silent, and this is how that is known.
        u64 Signature() const { return m_Signature; }

    private:
        u32 Add(u32 parent, Node node);
        void Walk(const ui::ViewTree& views, u32 view, u32 parent, const CaretSource& carets);

        std::vector<Node> m_Nodes;
        std::vector<Problem> m_Problems;
        std::vector<u32> m_ByView;      // view index -> node index
        u64 m_Signature = 0;
    };

    // The AT-SPI role a widget role is announced as.
    Role RoleFor(ui::Role role, bool multiline, bool password);

}
