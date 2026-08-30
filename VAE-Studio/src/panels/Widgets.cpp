#include "Widgets.h"

#include <cstring>

namespace vae::fields {

    namespace {
        f32 AsNumber(const doc::Value& value, f32 fallback) {
            if (const f32* n = std::get_if<f32>(&value)) return *n;
            if (const bool* b = std::get_if<bool>(&value)) return *b ? 1.0f : 0.0f;
            return fallback;
        }
    }

    bool Number(EditorState& state, Uuid node, const char* label, doc::Prop prop, f32 fallback,
                f32 speed, f32 min, f32 max) {
        f32 value = AsNumber(state.GetProp(node, prop), fallback);
        Label(label);
        ImGui::PushID(static_cast<int>(prop));
        const bool changed = ImGui::DragFloat("##v", &value, speed, min, max, "%.2f");
        if (changed) state.SetProp(node, prop, value);
        if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();
        ImGui::PopID();
        return changed;
    }

    bool Toggle(EditorState& state, Uuid node, const char* label, doc::Prop prop, bool fallback) {
        const doc::Value current = state.GetProp(node, prop);
        bool value = fallback;
        if (const bool* b = std::get_if<bool>(&current)) value = *b;
        Label(label);
        ImGui::PushID(static_cast<int>(prop));
        const bool changed = ImGui::Checkbox("##v", &value);
        if (changed) { state.SetProp(node, prop, value); state.EndGesture(); }
        ImGui::PopID();
        return changed;
    }

    bool Colour(EditorState& state, Uuid node, const char* label, doc::Prop prop, Color fallback) {
        return Colour(state, node, label, doc::PropName(prop), prop, fallback);
    }

    // `key` is what a write is filed under: the property itself, or a state overlay like
    // "hovered:fill". Both are one colour on one node as far as this field is concerned.
    bool Colour(EditorState& state, Uuid node, const char* label, std::string key,
                [[maybe_unused]] doc::Prop prop, Color fallback) {
        const doc::Value current = state.GetProp(node, key);
        Color value = fallback;
        const bool unset = !doc::IsSet(current);
        if (const Color* c = std::get_if<Color>(&current)) value = *c;

        // A token reference is shown as such rather than flattened into the colour it happens to
        // resolve to — overwriting it with a literal is a real edit, and has to look like one.
        const bool isToken = std::holds_alternative<doc::TokenRef>(current);
        if (isToken) {
            const doc::Value resolved = state.Doc().ResolveValue(current);
            if (const Color* c = std::get_if<Color>(&resolved)) value = *c;
        }

        Label(label);
        ImGui::PushID(key.c_str());

        bool changed = false;
        if (isToken) {
            // A swatch that opens the theme, so a token is something a designer can change rather
            // than something they are stuck with. "Custom" is the way out to a literal.
            const auto& token = std::get<doc::TokenRef>(current);
            if (ImGui::ColorButton("##token", ImVec4(value.r, value.g, value.b, value.a),
                                   ImGuiColorEditFlags_NoTooltip, ImVec2(20, 0)))
                ImGui::OpenPopup("##tokens");
            ImGui::SameLine();
            if (ImGui::SmallButton(token.name.c_str())) ImGui::OpenPopup("##tokens");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Themed as '%s' — click to change it", token.name.c_str());

            if (ImGui::BeginPopup("##tokens")) {
                for (const auto& [name, entry] : state.Doc().Tokens()) {
                    const doc::Value shade = state.Doc().ActiveTheme() == doc::Theme::Dark
                                           ? entry.dark : entry.light;
                    const Color* c = std::get_if<Color>(&shade);
                    if (!c) continue;
                    ImGui::PushID(name.c_str());
                    ImGui::ColorButton("##swatch", ImVec4(c->r, c->g, c->b, c->a),
                                       ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
                    ImGui::SameLine();
                    if (ImGui::Selectable(name.c_str(), name == token.name)) {
                        state.SetProp(node, key, doc::TokenRef{ name });
                        state.EndGesture();
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Selectable("Custom…")) {
                    // Unlinking keeps what it looked like, so the first thing that happens is not
                    // the node turning black.
                    state.SetProp(node, key, value);
                    state.EndGesture();
                    changed = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
            return changed;
        }

        changed = ImGui::ColorEdit4("##v", &value.r,
                                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        if (changed) {
            // Picking a colour on a property nobody has set yet means wanting to see it. Writing
            // the picker's alpha straight through would leave it invisible and look broken.
            if (unset && value.a <= 0.0f) value.a = 1.0f;
            state.SetProp(node, key, value);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();

        // A literal can be themed: the way back to a token, and the only route for a property the
        // library never tokenised.
        if (!state.Doc().Tokens().empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("T")) ImGui::OpenPopup("##tokens");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use a theme token");
            if (ImGui::BeginPopup("##tokens")) {
                for (const auto& [name, entry] : state.Doc().Tokens()) {
                    const doc::Value shade = state.Doc().ActiveTheme() == doc::Theme::Dark
                                           ? entry.dark : entry.light;
                    const Color* c = std::get_if<Color>(&shade);
                    if (!c) continue;
                    ImGui::PushID(name.c_str());
                    ImGui::ColorButton("##swatch", ImVec4(c->r, c->g, c->b, c->a),
                                       ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
                    ImGui::SameLine();
                    if (ImGui::Selectable(name.c_str())) {
                        state.SetProp(node, key, doc::TokenRef{ name });
                        state.EndGesture();
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
        }

        ImGui::PopID();
        return changed;
    }

    bool Text(EditorState& state, Uuid node, const char* label, doc::Prop prop,
              const char* hint, bool multiline) {
        const doc::Value current = state.GetProp(node, prop);
        std::string value;
        if (const auto* s = std::get_if<std::string>(&current)) value = *s;

        char buffer[1024];
        std::snprintf(buffer, sizeof buffer, "%s", value.c_str());

        Label(label);
        ImGui::PushID(static_cast<int>(prop));
        bool changed = false;
        if (multiline)
            changed = ImGui::InputTextMultiline("##v", buffer, sizeof buffer, ImVec2(-1.0f, 64.0f));
        else if (hint)
            changed = ImGui::InputTextWithHint("##v", hint, buffer, sizeof buffer);
        else
            changed = ImGui::InputText("##v", buffer, sizeof buffer);
        if (changed) state.SetProp(node, prop, std::string(buffer));
        if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();
        ImGui::PopID();
        return changed;
    }

    bool Choice(EditorState& state, Uuid node, const char* label, doc::Prop prop,
                const char* const* options, int count, int fallback) {
        const doc::Value current = state.GetProp(node, prop);
        int index = fallback;
        if (const auto* s = std::get_if<std::string>(&current)) {
            for (int i = 0; i < count; ++i)
                if (*s == options[i]) { index = i; break; }
        }
        Label(label);
        ImGui::PushID(static_cast<int>(prop));
        const bool changed = ImGui::Combo("##v", &index, options, count);
        if (changed) { state.SetProp(node, prop, std::string(options[index])); state.EndGesture(); }
        ImGui::PopID();
        return changed;
    }

    bool EnumField(const char* label, int& value, const char* const* options, int count) {
        Label(label);
        ImGui::PushID(label);
        const bool changed = ImGui::Combo("##v", &value, options, count);
        ImGui::PopID();
        return changed;
    }

    bool SizeField(const char* label, layout::Size& size) {
        static const char* kModes[] = { "Fixed", "Hug", "Fill", "Percent" };
        Label(label);
        ImGui::PushID(label);

        int mode = static_cast<int>(size.mode);
        const f32 full = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(full * 0.45f);
        bool changed = ImGui::Combo("##mode", &mode, kModes, 4);
        if (changed) size.mode = static_cast<layout::SizeMode>(mode);

        // Hug has no number to show, and a stale one sitting next to it reads as if it applied.
        if (size.mode != layout::SizeMode::Hug) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            const f32 speed = size.mode == layout::SizeMode::Percent ? 0.01f : 1.0f;
            changed |= ImGui::DragFloat("##value", &size.value, speed, 0.0f, 0.0f, "%.2f");
        }
        ImGui::PopID();
        return changed;
    }

    bool EdgesField(const char* label, Edges& edges) {
        Label(label);
        ImGui::PushID(label);
        f32 values[4] = { edges.left, edges.top, edges.right, edges.bottom };
        const bool changed = ImGui::DragFloat4("##v", values, 1.0f, 0.0f, 0.0f, "%.0f");
        if (changed) edges = Edges{ values[0], values[1], values[2], values[3] };
        ImGui::PopID();
        return changed;
    }

    bool Vec2Field(const char* label, Vec2& value, f32 speed) {
        Label(label);
        ImGui::PushID(label);
        const bool changed = ImGui::DragFloat2("##v", &value.x, speed, 0.0f, 0.0f, "%.1f");
        ImGui::PopID();
        return changed;
    }

}
