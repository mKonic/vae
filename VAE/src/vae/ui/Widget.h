#pragma once

#include "vae/doc/PropBag.h"

namespace vae::ui {

    // Which widget, exactly. A node id alone is not enough: place two instances of the same Button
    // component and both views report the component root as their source, so anything keyed on the
    // node — a caret, an open menu, a list's rows — would be shared between them.
    struct WidgetId {
        Uuid node = Uuid::Invalid();
        Uuid instance = Uuid::Invalid();

        WidgetId() = default;
        WidgetId(Uuid node_, Uuid instance_ = Uuid::Invalid()) : node(node_), instance(instance_) {}

        bool Valid() const { return node.Valid(); }
        bool operator==(const WidgetId&) const = default;
        auto operator<=>(const WidgetId&) const = default;
    };

    // What a node IS, behaviorally.
    //
    // A widget is a document plus a native behavior, and this is the seam between them: the
    // visuals are ordinary nodes a designer can open, restyle and rearrange, and the role is the
    // only thing that says "this styled box answers clicks like a button". Nothing about a
    // widget's appearance is compiled in.
    enum class Role : u16 {
        None = 0,
        Button, TextInput, Checkbox, Radio, Switch, Slider, Dropdown, DropdownItem,
        Tabs, Tab, Scroll, List, ListItem, Table, TableColumn,
        Modal, Popover, Toast, Router,
        // Disclosure, feedback and the two pointer-opened surfaces. Accordion is a marker: it owns
        // no behavior, it only says that the collapsibles under it are one at a time.
        Collapsible, Accordion, Progress, Splitter, Tooltip, ContextMenu, Menu, Pagination,
        Chart, InputOtp, Carousel, Combobox, Calendar,
        // Parts a behavior owns but a designer still styles: the slider's filled track and knob,
        // the checkbox's tick, the scrollbar's thumb.
        Track, Fill, Knob, Indicator, Thumb, Content, Anchor, Scrim,
        Count
    };

    const char* RoleName(Role role);
    std::optional<Role> RoleFromName(std::string_view name);

    // Interaction state. Set by behaviors, read by the visual layer.
    enum class StateBit : u16 {
        Hovered  = 1u << 0,
        Pressed  = 1u << 1,
        Focused  = 1u << 2,
        Disabled = 1u << 3,
        Checked  = 1u << 4,
        Selected = 1u << 5,
        Open     = 1u << 6,
    };
    using StateMask = u16;

    constexpr StateMask operator|(StateBit a, StateBit b) {
        return static_cast<StateMask>(static_cast<u16>(a) | static_cast<u16>(b));
    }
    constexpr bool HasState(StateMask mask, StateBit bit) {
        return (mask & static_cast<u16>(bit)) != 0;
    }
    constexpr StateMask WithState(StateMask mask, StateBit bit, bool on) {
        return on ? static_cast<StateMask>(mask | static_cast<u16>(bit))
                  : static_cast<StateMask>(mask & ~static_cast<u16>(bit));
    }

    const char* StateName(StateBit bit);

    // A per-state restyle lives on the node itself as a custom property named "<state>:<prop>",
    // e.g. "pressed:fill". That keeps the whole visual definition of a widget inside one node the
    // Inspector can edit, instead of in a stylesheet the designer never sees.
    std::string StateKey(StateBit bit, doc::Prop prop);

    // A state can say "the same colour, lighter" instead of naming one: "hovered:tint" = 0.10.
    // Naming a colour is a decision about *that* colour, so a button someone recoloured red still
    // hovers whatever the library authored; a tint is a decision about the widget, and follows.
    std::string StateTintKey(StateBit bit);
    // Positive mixes toward white, negative toward black. Alpha is left alone: tinting something
    // invisible must not make it visible.
    Color Tinted(Color colour, f32 amount);

    // Overlays every active state's properties onto `into`, weakest first, so a disabled hovered
    // control looks disabled and a pressed one beats its own hover styling.
    void ApplyStateOverlay(doc::PropBag& into, const doc::PropBag& source, StateMask state);

}
