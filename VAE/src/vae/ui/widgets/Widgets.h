#pragma once

#include "vae/ui/UiHost.h"

namespace vae::ui::widgets {

    Scope<Behavior> MakeButton();
    Scope<Behavior> MakeTextInput();
    Scope<Behavior> MakeCheckbox();
    Scope<Behavior> MakeRadio();
    Scope<Behavior> MakeSwitch();
    Scope<Behavior> MakeSlider();
    Scope<Behavior> MakeDropdown();
    Scope<Behavior> MakeDropdownItem();
    Scope<Behavior> MakeTabs();
    Scope<Behavior> MakeTab();
    Scope<Behavior> MakeScroll();
    Scope<Behavior> MakeList();
    Scope<Behavior> MakeTable();
    Scope<Behavior> MakeThumb();
    Scope<Behavior> MakeModal();
    Scope<Behavior> MakePopover();
    Scope<Behavior> MakeToast();
    Scope<Behavior> MakeScrim();
    Scope<Behavior> MakeRouter();
    Scope<Behavior> MakeCollapsible();
    Scope<Behavior> MakeProgress();
    Scope<Behavior> MakeSplitter();
    Scope<Behavior> MakeTooltip();
    Scope<Behavior> MakeContextMenu();
    Scope<Behavior> MakeMenu();
    Scope<Behavior> MakePagination();
    Scope<Behavior> MakeChart();
    Scope<Behavior> MakeInputOtp();
    Scope<Behavior> MakeCarousel();
    Scope<Behavior> MakeCombobox();
    Scope<Behavior> MakeCalendar();

    // --- shared helpers -------------------------------------------------------------------------

    inline Vec2 PointOf(const Event& event) {
        switch (event.type) {
            case EventType::MouseMoved: return { event.mouse.x, event.mouse.y };
            case EventType::MouseButtonPressed:
            case EventType::MouseButtonReleased: return { event.button.x, event.button.y };
            default: return { 0.0f, 0.0f };
        }
    }

    inline bool IsLeftPress(const Event& event) {
        return event.type == EventType::MouseButtonPressed && event.button.button == Mouse::Left;
    }
    inline bool IsLeftRelease(const Event& event) {
        return event.type == EventType::MouseButtonReleased && event.button.button == Mouse::Left;
    }

    inline void Fire(WidgetContext& context, ActionKind kind, doc::Value value = {}) {
        const ViewTree::View& view = context.Self();
        context.host.Emit({ kind, view.sourceId, view.instanceId, view.name, std::move(value) });

        // Declared navigation: a `goTo` on the widget says where a click leads, and no script is
        // needed for the case that needs no logic. Wiring two screens together should not require
        // opening the editor — and the action is still emitted, so a script can also see the click.
        if (kind != ActionKind::Clicked) return;
        const doc::Value target = context.tree.ResolvedProp(context.view, doc::Prop::GoTo);
        if (doc::TypeOf(target) != doc::ValueType::Text) return;
        const std::string& where = std::get<std::string>(target);
        if (where.empty()) return;
        // Queued rather than performed: the click is delivered to whatever is listening first, and
        // only then does the screen change. "back" is the one destination that is not a screen name.
        context.host.RequestNavigation(where);
    }

    // A disabled control still eats the click. Letting it fall through to whatever sits behind is
    // how a greyed-out button ends up dragging the panel it is drawn on.
    inline bool SwallowedWhileDisabled(const WidgetContext& context, const Event& event) {
        return !context.Enabled() && (event.IsMouse() || event.IsKeyboard())
            && event.type != EventType::MouseMoved;
    }

    // Position of a view among its same-role siblings under `container`.
    u32 IndexAmongRole(const ViewTree& tree, u32 container, u32 view, Role role);

    // Nearest ancestor (inclusive) carrying this role.
    u32 AncestorWithRole(const ViewTree& tree, u32 view, Role role);

    // The node a widget shows its current text on: a descendant explicitly named "Label" if there
    // is one, otherwise the first Text node that is not part of the widget's own menu content.
    u32 LabelOf(const ViewTree& tree, u32 view);

}
