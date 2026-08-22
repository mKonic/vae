#include "Panels.h"
#include "Widgets.h"

#include "vae/ui/Widget.h"

#include <imgui.h>

namespace vae {

    namespace {

        const char* const kLayoutModes[] = { "Absolute", "Stack" };
        const char* const kAxes[]        = { "Row", "Column" };
        const char* const kAligns[]      = { "Start", "Center", "End", "Stretch" };
        const char* const kJustifies[]   = { "Start", "Center", "End", "Space between",
                                             "Space around", "Space evenly" };
        const char* const kConstraints[] = { "Start", "End", "Both", "Center", "Scale" };
        const char* const kTextAligns[]  = { "left", "center", "right" };
        const char* const kWraps[]       = { "word", "char", "none" };

        void SectionHeader(const char* title) {
            ImGui::SeparatorText(title);
        }

        // What a property actually resolves to right now, token or literal — the sensible place for
        // a state's colour to start from.
        Color ColourOf(EditorState& state, Uuid id, doc::Prop prop, Color fallback) {
            const doc::Value resolved = state.Doc().ResolveValue(state.GetProp(id, prop));
            const Color* value = std::get_if<Color>(&resolved);
            return value ? *value : fallback;
        }

        void DrawLayoutSection(EditorState& state, Uuid id, const doc::Node& node) {
            layout::LayoutStyle style = node.layout;
            bool changed = false;

            int mode = static_cast<int>(style.mode);
            if (fields::EnumField("Layout", mode, kLayoutModes, 2)) {
                style.mode = static_cast<layout::LayoutMode>(mode);
                changed = true;
            }

            if (style.mode == layout::LayoutMode::Stack) {
                int axis = static_cast<int>(style.axis);
                if (fields::EnumField("Direction", axis, kAxes, 2)) {
                    style.axis = static_cast<layout::Axis>(axis);
                    changed = true;
                }
                fields::Label("Gap");
                changed |= ImGui::DragFloat("##gap", &style.gap, 1.0f, 0.0f, 0.0f, "%.0f");
                changed |= fields::EdgesField("Padding", style.padding);

                int align = static_cast<int>(style.align);
                if (fields::EnumField("Align", align, kAligns, 4)) {
                    style.align = static_cast<layout::Align>(align);
                    changed = true;
                }
                int justify = static_cast<int>(style.justify);
                if (fields::EnumField("Justify", justify, kJustifies, 6)) {
                    style.justify = static_cast<layout::Justify>(justify);
                    changed = true;
                }
                fields::Label("Wrap");
                changed |= ImGui::Checkbox("##wrap", &style.wrap);
            }

            changed |= fields::SizeField("Width", style.width);
            changed |= fields::SizeField("Height", style.height);

            // Placement only means anything inside an absolute parent; a stack decides it instead.
            const doc::Node* parent = state.Doc().Find(node.parent);
            const bool absoluteParent = !parent || parent->layout.mode == layout::LayoutMode::Absolute;
            if (absoluteParent) {
                changed |= fields::Vec2Field("Position", style.offsetStart);
                int cx = static_cast<int>(style.constraintX);
                if (fields::EnumField("Pin X", cx, kConstraints, 5)) {
                    style.constraintX = static_cast<layout::Constraint>(cx);
                    changed = true;
                }
                int cy = static_cast<int>(style.constraintY);
                if (fields::EnumField("Pin Y", cy, kConstraints, 5)) {
                    style.constraintY = static_cast<layout::Constraint>(cy);
                    changed = true;
                }
                if (style.constraintX == layout::Constraint::End
                    || style.constraintX == layout::Constraint::StartEnd
                    || style.constraintY == layout::Constraint::End
                    || style.constraintY == layout::Constraint::StartEnd)
                    changed |= fields::Vec2Field("Inset end", style.offsetEnd);
            }

            if (ImGui::TreeNode("Constraints##minmax")) {
                changed |= fields::Vec2Field("Min size", style.minSize);
                fields::Label("Aspect");
                changed |= ImGui::DragFloat("##aspect", &style.aspectRatio, 0.01f, 0.0f, 0.0f, "%.3f");
                ImGui::TreePop();
            }

            if (changed) state.SetLayout(id, style);
            if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();
        }

        void DrawStyleSection(EditorState& state, Uuid id) {
            fields::Colour(state, id, "Fill", doc::Prop::Fill, { 0, 0, 0, 0 });
            fields::Colour(state, id, "Stroke", doc::Prop::Stroke, { 0, 0, 0, 0 });
            fields::Number(state, id, "Stroke width", doc::Prop::StrokeWidth, 0.0f, 0.1f, 0.0f, 64.0f);
            fields::Number(state, id, "Corner radius", doc::Prop::CornerRadius, 0.0f, 0.5f, 0.0f, 512.0f);
            fields::Number(state, id, "Opacity", doc::Prop::Opacity, 1.0f, 0.01f, 0.0f, 1.0f);
            fields::Toggle(state, id, "Clip content", doc::Prop::ClipContent, false);

            // One of its children at a time, by name. Loading, failed, empty and the content are
            // four drawings of one screen, and this is how a designer looks at each of them
            // without running anything — and how a script says which one is true right now.
            if (const doc::Node* node = state.Doc().Find(id); node && !node->children.empty()) {
                std::vector<const char*> names{ "(everything)" };
                for (const Uuid child : node->children)
                    if (const doc::Node* c = state.Doc().Find(child)) names.push_back(c->name.c_str());

                const doc::Value value = state.GetProp(id, doc::Prop::Shown);
                const std::string* held = std::get_if<std::string>(&value);
                const std::string current = held ? *held : std::string{};
                int index = 0;
                for (int i = 1; i < static_cast<int>(names.size()); ++i)
                    if (current == names[static_cast<std::size_t>(i)]) { index = i; break; }

                fields::Label("Shows");
                ImGui::PushID("shown");
                if (ImGui::Combo("##v", &index, names.data(), static_cast<int>(names.size()))) {
                    state.SetProp(id, doc::Prop::Shown,
                                  index == 0 ? std::string{} : std::string(names[static_cast<std::size_t>(index)]));
                    state.EndGesture();
                }
                ImGui::PopID();

                // How many times its first child is drawn. One row styled by hand and a number
                // here is the whole of "this list has data in it"; a script sets the same number.
                fields::Number(state, id, "Repeat", doc::Prop::Repeat, 0.0f, 1.0f, 0.0f, 2000.0f);
            }

            // What it looks like while the pointer is on it. Without this a button recoloured red
            // still hovers whatever colour the library authored, because "hovered:fill" is a
            // property of its own and nothing was re-deriving it.
            if (ImGui::TreeNode("States")) {
                static const ui::StateBit kStates[] = {
                    ui::StateBit::Hovered, ui::StateBit::Pressed, ui::StateBit::Focused,
                    ui::StateBit::Disabled, ui::StateBit::Checked, ui::StateBit::Selected,
                };
                for (const ui::StateBit bit : kStates) {
                    const char* name = ui::StateName(bit);
                    if (!ImGui::TreeNode(name)) continue;
                    ImGui::PushID(name);

                    // Unset states inherit the base colour, so the swatch starts from what the node
                    // already looks like rather than from black.
                    const Color baseFill = ColourOf(state, id, doc::Prop::Fill, { 0, 0, 0, 0 });
                    const Color baseText = ColourOf(state, id, doc::Prop::TextColor, { 1, 1, 1, 1 });

                    fields::Colour(state, id, "Fill", ui::StateKey(bit, doc::Prop::Fill),
                                   doc::Prop::Fill, baseFill);
                    fields::Colour(state, id, "Stroke", ui::StateKey(bit, doc::Prop::Stroke),
                                   doc::Prop::Stroke, { 0, 0, 0, 0 });
                    fields::Colour(state, id, "Text", ui::StateKey(bit, doc::Prop::TextColor),
                                   doc::Prop::TextColor, baseText);

                    // Naming a colour is a decision about that colour; a tint is a decision about
                    // the widget, and follows whatever its fill becomes.
                    const std::string tintKey = ui::StateTintKey(bit);
                    const doc::Value stored = state.GetProp(id, tintKey);
                    f32 tint = 0.0f;
                    if (const f32* value = std::get_if<f32>(&stored)) tint = *value;
                    fields::Label("Tint");
                    if (ImGui::SliderFloat("##tint", &tint, -0.5f, 0.5f, "%+.2f"))
                        state.SetProp(id, tintKey, tint == 0.0f ? doc::Value{} : doc::Value{ tint });
                    if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Lighter or darker than the fill, whatever the fill is");

                    // Clearing is the way back to "same as the base", which is the answer after
                    // recolouring a widget and wanting its hover to follow.
                    if (ImGui::SmallButton("Clear")) {
                        state.SetProp(id, ui::StateKey(bit, doc::Prop::Fill), doc::Value{});
                        state.SetProp(id, ui::StateKey(bit, doc::Prop::Stroke), doc::Value{});
                        state.SetProp(id, ui::StateKey(bit, doc::Prop::TextColor), doc::Value{});
                        state.EndGesture();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Follow the base colours again");

                    ImGui::PopID();
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }

            if (ImGui::TreeNode("Shadow")) {
                fields::Colour(state, id, "Colour", doc::Prop::ShadowColor, { 0, 0, 0, 0 });
                fields::Number(state, id, "Blur", doc::Prop::ShadowBlur, 12.0f, 0.5f, 0.0f, 256.0f);
                fields::Number(state, id, "Spread", doc::Prop::ShadowSpread, 0.0f, 0.5f, -64.0f, 64.0f);
                ImGui::TreePop();
            }
        }

        void DrawTextSection(EditorState& state, Uuid id) {
            fields::Text(state, id, "Content", doc::Prop::Text, nullptr, true);
            fields::Text(state, id, "Family", doc::Prop::FontFamily, "default");
            fields::Number(state, id, "Size", doc::Prop::FontSize, 14.0f, 0.5f, 1.0f, 512.0f);
            fields::Number(state, id, "Weight", doc::Prop::FontWeight, 400.0f, 10.0f, 100.0f, 900.0f);
            fields::Toggle(state, id, "Italic", doc::Prop::FontItalic, false);
            fields::Colour(state, id, "Colour", doc::Prop::TextColor, { 1, 1, 1, 1 });
            fields::Choice(state, id, "Align", doc::Prop::TextAlign, kTextAligns, 3, 0);
            fields::Choice(state, id, "Wrap", doc::Prop::TextWrap, kWraps, 3, 0);
            fields::Number(state, id, "Line height", doc::Prop::LineHeight, 0.0f, 0.5f, 0.0f, 256.0f);
            fields::Number(state, id, "Letter spacing", doc::Prop::LetterSpacing, 0.0f, 0.1f, -16.0f, 32.0f);
            // A tick box rather than a different component: any label you have already styled can
            // be one the reader is allowed to select and copy out of.
            fields::Toggle(state, id, "Selectable", doc::Prop::Selectable, false);
        }

        // Only the properties the node's role actually uses. Showing every widget property on every
        // node turns the Inspector into a wall nobody reads.
        void DrawWidgetSection(EditorState& state, Uuid id, ui::Role role) {
            fields::Toggle(state, id, "Enabled", doc::Prop::Enabled, true);
            switch (role) {
                case ui::Role::Checkbox:
                case ui::Role::Switch:
                    fields::Toggle(state, id, "Checked", doc::Prop::Checked, false);
                    break;
                case ui::Role::Radio:
                    fields::Toggle(state, id, "Checked", doc::Prop::Checked, false);
                    fields::Text(state, id, "Group", doc::Prop::Group, "group name");
                    break;
                case ui::Role::Slider:
                    fields::Number(state, id, "Value", doc::Prop::Value, 0.0f, 0.01f);
                    fields::Number(state, id, "Min", doc::Prop::MinValue, 0.0f, 0.1f);
                    fields::Number(state, id, "Max", doc::Prop::MaxValue, 1.0f, 0.1f);
                    fields::Number(state, id, "Step", doc::Prop::Step, 0.0f, 0.01f, 0.0f, 1000.0f);
                    break;
                case ui::Role::TextInput:
                    fields::Text(state, id, "Value", doc::Prop::Text);
                    fields::Text(state, id, "Placeholder", doc::Prop::Placeholder, "hint");
                    fields::Toggle(state, id, "Multiline", doc::Prop::Multiline, false);
                    fields::Toggle(state, id, "Password", doc::Prop::Password, false);
                    fields::Toggle(state, id, "Read only", doc::Prop::ReadOnly, false);
                    fields::Number(state, id, "Max length", doc::Prop::MaxLength, 0.0f, 1.0f, 0.0f, 4096.0f);
                    break;
                case ui::Role::Dropdown:
                case ui::Role::Tabs:
                    fields::Number(state, id, "Selected", doc::Prop::SelectedIndex, -1.0f, 1.0f, -1.0f, 999.0f);
                    break;
                case ui::Role::List:
                case ui::Role::Table:
                    fields::Number(state, id, "Row height", doc::Prop::ItemHeight, 28.0f, 1.0f, 1.0f, 512.0f);
                    fields::Number(state, id, "Row count", doc::Prop::ItemCount, 0.0f, 1.0f, 0.0f, 1.0e7f);
                    fields::Number(state, id, "Selected", doc::Prop::SelectedIndex, -1.0f, 1.0f, -1.0f, 1.0e7f);
                    break;
                case ui::Role::Router:
                    fields::Text(state, id, "Route", doc::Prop::Route, "screen name");
                    break;
                default:
                    break;
            }
        }

    }

    void DrawInspectorPanel(EditorState& state) {
        ImGui::Begin("Inspector###Inspector");

        const Uuid id = state.Primary();
        const doc::Node* node = state.Doc().Find(id);
        if (!node) {
            ImGui::TextDisabled("Nothing selected.");
            ImGui::End();
            return;
        }
        if (state.Selection().size() > 1)
            ImGui::TextDisabled("%zu selected — editing %s", state.Selection().size(),
                                node->name.c_str());

        char name[128];
        std::snprintf(name, sizeof name, "%s", node->name.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##name", name, sizeof name)) state.Rename(id, name);
        if (ImGui::IsItemDeactivatedAfterEdit()) state.EndGesture();

        const doc::Node* master = node->IsInstance() ? state.Doc().Find(node->componentId) : nullptr;
        ImGui::TextDisabled("%s%s%s", doc::NodeKindName(node->kind),
                            master ? " of " : "", master ? master->name.c_str() : "");

        // Which instance this part belongs to, and the way back out. A node authored inside a
        // component is on the screen once per copy, so "which one am I editing" is not a detail.
        if (!state.InstancePath().empty()) {
            ImGui::Separator();
            std::string trail;
            for (const Uuid instance : state.InstancePath()) {
                if (const doc::Node* step = state.Doc().Find(instance)) {
                    if (!trail.empty()) trail += "  ›  ";
                    trail += step->name;
                }
            }
            ImGui::TextDisabled("in %s", trail.c_str());
            if (ImGui::SmallButton("Leave")) state.ExitInstance();
            ImGui::SameLine();
            ImGui::TextDisabled("edits only this copy");

            // The name a script uses for it. Two identically-named parts are only distinguishable by
            // their path, so the Inspector is where a designer finds out what to type.
            const std::string path = state.ScriptPath(id);
            if (!path.empty()) {
                char script[192];
                std::snprintf(script, sizeof script, "%s", path.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                ImGui::InputText("##scriptpath", script, sizeof script,
                                 ImGuiInputTextFlags_ReadOnly);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("The name a script addresses this by, from %s",
                                      trail.c_str());
            }
            ImGui::Separator();
        }

        // What the node behaves as. Reading it through the override table matters: an instance's
        // role comes from its component unless this instance overrode it.
        const doc::Value roleValue = state.GetProp(id, doc::Prop::Role);
        ui::Role role = ui::Role::None;
        if (const auto* text = std::get_if<std::string>(&roleValue))
            role = ui::RoleFromName(*text).value_or(ui::Role::None);
        if (role != ui::Role::None) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.36f, 0.51f, 0.89f, 1.0f), "· %s", ui::RoleName(role));
        }

        if (ImGui::CollapsingHeader("Layout", ImGuiTreeNodeFlags_DefaultOpen))
            DrawLayoutSection(state, id, *node);

        // What a screen's width and height actually mean, which is not what the Layout section
        // implies. They are the artboard: what the canvas draws and the size the window opens at.
        // Whether the running app is allowed to be a different size is a separate question, and
        // one that has to be asked here because the answer changes what the numbers are for.
        if (node->kind == doc::NodeKind::Screen
            && ImGui::CollapsingHeader("Screen", ImGuiTreeNodeFlags_DefaultOpen)) {
            const bool fluid = state.Doc().Find(id)->props.Flag(doc::Prop::Resizable, true);
            fields::Toggle(state, id, "Resizable", doc::Prop::Resizable, true);
            if (fluid) {
                ImGui::TextDisabled("The app fills its window and lays out again when it");
                ImGui::TextDisabled("changes. The size above is where the design starts.");
            } else {
                ImGui::TextDisabled("A hard resolution: the window opens at exactly the");
                ImGui::TextDisabled("size above and cannot be dragged.");
            }
        }

        if (ImGui::CollapsingHeader("Style", ImGuiTreeNodeFlags_DefaultOpen))
            DrawStyleSection(state, id);
        if (node->kind == doc::NodeKind::Text && ImGui::CollapsingHeader("Text",
                                                                        ImGuiTreeNodeFlags_DefaultOpen))
            DrawTextSection(state, id);
        if (role != ui::Role::None && ImGui::CollapsingHeader("Widget",
                                                              ImGuiTreeNodeFlags_DefaultOpen))
            DrawWidgetSection(state, id, role);

        // Where a click leads. The common case needs no code at all, and a designer wiring two
        // screens together should not have to open the editor to do it.
        if (role == ui::Role::Button || role == ui::Role::ListItem || role == ui::Role::Tab) {
            if (ImGui::CollapsingHeader("Navigation", ImGuiTreeNodeFlags_DefaultOpen)) {
                const doc::Value target = state.GetProp(id, doc::Prop::GoTo);
                const std::string current = doc::TypeOf(target) == doc::ValueType::Text
                                          ? std::get<std::string>(target) : std::string{};

                ImGui::TextDisabled("On click, go to");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##goto", current.empty() ? "nowhere" : current.c_str())) {
                    if (ImGui::Selectable("nowhere", current.empty()))
                        state.SetProp(id, doc::Prop::GoTo, doc::Value{});
                    if (ImGui::Selectable("back", current == "back"))
                        state.SetProp(id, doc::Prop::GoTo, std::string("back"));
                    ImGui::Separator();
                    for (const Uuid screen : state.Screens()) {
                        const doc::Node* target2 = state.Doc().Find(screen);
                        if (!target2) continue;
                        if (ImGui::Selectable(target2->name.c_str(), current == target2->name))
                            state.SetProp(id, doc::Prop::GoTo, target2->name);
                    }
                    ImGui::EndCombo();
                }
                if (!current.empty())
                    ImGui::TextDisabled("The click still reaches the script as well.");
            }
        }

        if (node->IsInstance() && !node->overrides.empty()) {
            SectionHeader("Overrides");
            for (const auto& [target, bag] : node->overrides) {
                const doc::Node* inner = state.Doc().Find(target);
                for (const auto& [prop, value] : bag.Known()) {
                    ImGui::BulletText("%s · %s", inner ? inner->name.c_str() : "?",
                                      doc::PropName(prop));
                    (void)value;
                }
            }
            if (ImGui::SmallButton("Reset all overrides")) {
                state.Commands().BeginTransaction("Reset overrides");
                for (const auto& [target, bag] : node->overrides)
                    for (const auto& [prop, value] : bag.Known()) {
                        (void)value;
                        state.Execute(CreateScope<doc::SetOverrideCommand>(id, target, prop,
                                                                           doc::Value{}));
                    }
                state.Commands().EndTransaction(state.Doc());
                state.EndGesture();
            }
        }

        ImGui::End();
    }

}
