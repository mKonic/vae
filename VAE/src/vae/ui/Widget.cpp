#include "vaepch.h"
#include "vae/ui/Widget.h"

#include <algorithm>
#include <array>

namespace vae::ui {

    namespace {
        constexpr std::array<const char*, static_cast<std::size_t>(Role::Count)> kRoleNames{
            "none",
            "button", "textInput", "checkbox", "radio", "switch", "slider", "dropdown",
            "dropdownItem", "tabs", "tab", "scroll", "list", "listItem", "table", "tableColumn",
            "modal", "popover", "toast", "router",
            "collapsible", "accordion", "progress", "splitter", "tooltip", "contextMenu",
            "menu", "pagination", "chart", "inputOtp", "carousel", "combobox",
            "calendar",
            "track", "fill", "knob", "indicator", "thumb", "content", "anchor", "scrim",
        };

        // Weakest first. Order is the whole contract: a pressed control must not read as merely
        // hovered, and a disabled one must not read as pressed.
        constexpr std::array<StateBit, 7> kOverlayOrder{
            StateBit::Focused, StateBit::Selected, StateBit::Checked, StateBit::Open,
            StateBit::Hovered, StateBit::Pressed, StateBit::Disabled,
        };
    }

    const char* RoleName(Role role) {
        const auto index = static_cast<std::size_t>(role);
        return index < kRoleNames.size() ? kRoleNames[index] : "none";
    }

    std::optional<Role> RoleFromName(std::string_view name) {
        for (std::size_t i = 0; i < kRoleNames.size(); ++i)
            if (name == kRoleNames[i]) return static_cast<Role>(i);
        return std::nullopt;
    }

    const char* StateName(StateBit bit) {
        switch (bit) {
            case StateBit::Hovered:  return "hovered";
            case StateBit::Pressed:  return "pressed";
            case StateBit::Focused:  return "focused";
            case StateBit::Disabled: return "disabled";
            case StateBit::Checked:  return "checked";
            case StateBit::Selected: return "selected";
            case StateBit::Open:     return "open";
        }
        return "unknown";
    }

    std::string StateKey(StateBit bit, doc::Prop prop) {
        return std::string(StateName(bit)) + ':' + doc::PropName(prop);
    }

    std::string StateTintKey(StateBit bit) {
        return std::string(StateName(bit)) + ":tint";
    }

    Color Tinted(Color colour, f32 amount) {
        const f32 t = std::clamp(std::abs(amount), 0.0f, 1.0f);
        const f32 target = amount >= 0.0f ? 1.0f : 0.0f;
        return { colour.r + (target - colour.r) * t,
                 colour.g + (target - colour.g) * t,
                 colour.b + (target - colour.b) * t,
                 colour.a };
    }

    void ApplyStateOverlay(doc::PropBag& into, const doc::PropBag& source, StateMask state) {
        if (state == 0 || source.Custom().empty()) return;

        for (StateBit bit : kOverlayOrder) {
            if (!HasState(state, bit)) continue;
            const std::string prefix = std::string(StateName(bit)) + ':';
            for (const auto& [key, value] : source.Custom()) {
                if (!key.starts_with(prefix)) continue;
                const std::string_view name{ key.data() + prefix.size(), key.size() - prefix.size() };
                if (auto prop = doc::PropFromName(name)) into.Set(*prop, value);
                else into.Set(std::string(name), value);
            }
        }
    }

}
