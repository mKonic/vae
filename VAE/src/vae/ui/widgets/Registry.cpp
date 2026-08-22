#include "vaepch.h"
#include "vae/ui/widgets/Widgets.h"

namespace vae::ui {

    Scope<Behavior> MakeBehavior(Role role) {
        switch (role) {
            case Role::Button:       return widgets::MakeButton();
            case Role::TextInput:    return widgets::MakeTextInput();
            case Role::Checkbox:     return widgets::MakeCheckbox();
            case Role::Radio:        return widgets::MakeRadio();
            case Role::Switch:       return widgets::MakeSwitch();
            case Role::Slider:       return widgets::MakeSlider();
            case Role::Dropdown:     return widgets::MakeDropdown();
            case Role::DropdownItem: return widgets::MakeDropdownItem();
            case Role::Tabs:         return widgets::MakeTabs();
            case Role::Tab:          return widgets::MakeTab();
            case Role::Scroll:       return widgets::MakeScroll();
            case Role::List:         return widgets::MakeList();
            case Role::Table:        return widgets::MakeTable();
            case Role::Thumb:        return widgets::MakeThumb();
            case Role::Modal:        return widgets::MakeModal();
            case Role::Popover:      return widgets::MakePopover();
            case Role::Toast:        return widgets::MakeToast();
            case Role::Scrim:        return widgets::MakeScrim();
            case Role::Router:       return widgets::MakeRouter();
            case Role::Collapsible:  return widgets::MakeCollapsible();
            case Role::Progress:     return widgets::MakeProgress();
            case Role::Splitter:     return widgets::MakeSplitter();
            case Role::Tooltip:      return widgets::MakeTooltip();
            case Role::ContextMenu:  return widgets::MakeContextMenu();
            case Role::Menu:         return widgets::MakeMenu();
            case Role::Pagination:   return widgets::MakePagination();
            case Role::Chart:        return widgets::MakeChart();
            case Role::InputOtp:     return widgets::MakeInputOtp();
            case Role::Carousel:     return widgets::MakeCarousel();
            case Role::Combobox:     return widgets::MakeCombobox();
            case Role::Calendar:     return widgets::MakeCalendar();

            // Roles that only mark a part for its owner to find — a slider's knob, a checkbox's
            // tick, a tab panel. They are addressed, never interactive on their own.
            default: return nullptr;
        }
    }

}
