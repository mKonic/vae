#include "Panels.h"

#include "../ScriptSession.h"

#include "vae/base/Log.h"
#include "vae/doc/Command.h"
#include "vae/doc/Blueprint.h"
#include "vae/script/BlueprintHost.h"

#include "NodeShape.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_node_editor.h>
#include <widgets.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

namespace vae {

    namespace {

        using Builder = blueprint::NodeBuilder;

        // Unreal's palette, near enough that someone who has drawn a Blueprint reads this without
        // being told: red is something that happened, blue is something that happens, green is
        // something that is worked out, grey is the shape of the thing.
        ImVec4 CategoryColour(doc::BlueprintCategory category) {
            switch (category) {
                case doc::BlueprintCategory::Event:    return { 0.62f, 0.16f, 0.16f, 1.0f };
                case doc::BlueprintCategory::Flow:     return { 0.28f, 0.29f, 0.34f, 1.0f };
                case doc::BlueprintCategory::Variable: return { 0.18f, 0.36f, 0.28f, 1.0f };
                case doc::BlueprintCategory::Widget:   return { 0.13f, 0.31f, 0.52f, 1.0f };
                case doc::BlueprintCategory::App:      return { 0.24f, 0.24f, 0.48f, 1.0f };
                case doc::BlueprintCategory::Data:     return { 0.16f, 0.40f, 0.22f, 1.0f };
                case doc::BlueprintCategory::Service:  return { 0.11f, 0.36f, 0.38f, 1.0f };
                case doc::BlueprintCategory::Count:    break;
            }
            return { 0.3f, 0.3f, 0.3f, 1.0f };
        }

        // A wire is the colour of what it carries. Same idea as Unreal's, and the same reason: the
        // type of a value is readable from across the blueprint without reading a single label.
        ImVec4 PinColour(doc::PinType type) {
            switch (type) {
                case doc::PinType::Exec:   return { 0.90f, 0.90f, 0.90f, 1.0f };
                case doc::PinType::Bool:   return { 0.85f, 0.30f, 0.30f, 1.0f };
                case doc::PinType::Number: return { 0.61f, 0.87f, 0.32f, 1.0f };
                case doc::PinType::Text:   return { 0.95f, 0.38f, 0.76f, 1.0f };
                case doc::PinType::Colour: return { 0.44f, 0.58f, 0.96f, 1.0f };
                case doc::PinType::Any:    return { 0.78f, 0.78f, 0.78f, 1.0f };
            }
            return { 1, 1, 1, 1 };
        }

        // Pin ids live in their own space in the editor, so they only have to be unique among
        // themselves. Node id in the high half, the pin's side and index in the low half — which
        // means a node may have as many pins as it likes and there are as many nodes as a u32 has.
        constexpr ed::PinId PinId(u32 node, bool input, std::size_t index) {
            return static_cast<ed::PinId>((static_cast<u64>(node) << 32)
                                          | (input ? 0u : 0x8000'0000u)
                                          | static_cast<u32>(index));
        }
        constexpr u32 PinNode(ed::PinId id) {
            return static_cast<u32>(static_cast<u64>(id.Get()) >> 32);
        }
        constexpr bool PinIsInput(ed::PinId id) {
            return (static_cast<u64>(id.Get()) & 0x8000'0000u) == 0;
        }
        constexpr std::size_t PinIndex(ed::PinId id) {
            return static_cast<std::size_t>(static_cast<u64>(id.Get()) & 0x7fff'ffffu);
        }

        struct Editing {
            ed::EditorContext* context = nullptr;
            Uuid owner = Uuid::Invalid();       // the screen or component this blueprint drives
            doc::Blueprint working;                 // what the canvas is showing, before it is committed
            bool positioned = false;            // whether the editor has been told where nodes are
            // The frame the positions were pushed in. Nothing is read back that frame: a node the
            // editor has not laid out yet has no size, and reading a zero back would write it into
            // the document — which is what collapsed every comment box to a line.
            bool seeding = false;
            bool fitPending = false;            // frame the whole blueprint the first time it is shown
            u64 seenRevision = 0;

            // The node search, whether it was opened from the background or by letting go of a
            // wire in empty space. A wire that ends nowhere is how a Blueprint is actually drawn —
            // you pull from a pin and say what you want next — so the drop is remembered and the
            // new node is wired up the moment it is chosen.
            bool searchOpen = false;
            char search[96]{};
            Vec2 searchAt{};
            ed::PinId dropped = 0;

            std::string newVariable;
            doc::ValueType newVariableType = doc::ValueType::Number;
            std::string renaming;
            u32 focusNode = 0;                  // a node the error list asked to be shown
            // What the running app went through since the last frame. Taken once and kept for the
            // length of this one, because the canvas is drawn once and every link is announced to
            // the editor as it is drawn.
            std::vector<script::BlueprintHost::Flow> flowThisFrame;
        };

        Editing& Ed() { static Editing editing; return editing; }

        // ------------------------------------------------------------------------- the document

        // Which thing's logic is being edited: the component open on the canvas, or the screen.
        Uuid OwnerOf(EditorState& state) {
            const Uuid component = state.EditingComponent();
            return component.Valid() ? component : state.ActiveScreen();
        }

        std::string NameOf(EditorState& state, Uuid owner) {
            const doc::Node* node = state.Doc().Find(owner);
            return node ? node->name : std::string();
        }

        // Every named node inside the thing this blueprint drives — what a widget pin offers, and the
        // reason those pins are a dropdown rather than a place to mistype a name.
        std::vector<std::string> NodeNames(EditorState& state, Uuid owner) {
            std::vector<std::string> names;
            if (!owner.Valid()) return names;
            for (const Uuid id : state.Doc().Subtree(owner)) {
                if (id == owner) continue;
                const doc::Node* node = state.Doc().Find(id);
                if (node && !node->name.empty()) names.push_back(node->name);
            }
            std::ranges::sort(names);
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return names;
        }

        // Writes the working copy back through the command stack, so every edit is one undo and a
        // drag is one undo rather than one per mouse-move.
        void Commit(EditorState& state, std::string name) {
            if (!Ed().owner.Valid()) return;
            state.Commands().Execute(state.Doc(),
                CreateScope<doc::SetBlueprintCommand>(Ed().owner, Ed().working, std::move(name)));
            Ed().seenRevision = state.Doc().Revision();
        }

        // ------------------------------------------------------------------------- literals

        // What a fixed pin offers instead of a free text field. A widget pin is a list of the
        // widgets that are actually there, and a route is a list of the screens that exist —
        // which is the difference between a blueprint that runs and one with a typo in it.
        std::vector<std::string> ChoicesFor(EditorState& state, const doc::BlueprintNode& node,
                                            const doc::PinSpec& pin) {
            if (pin.name == "Node" || pin.name == "List") return NodeNames(state, Ed().owner);
            if (node.type == "app.navigate" && pin.name == "Route") {
                std::vector<std::string> screens;
                for (const Uuid id : state.Doc().Screens())
                    if (const doc::Node* screen = state.Doc().Find(id); screen && !screen->name.empty())
                        screens.push_back(screen->name);
                return screens;
            }
            if (node.type == "app.log" && pin.name == "Level")
                return { "trace", "info", "warn", "error" };
            return {};
        }

        // The little editor a pin carries when nothing is wired to it. Unreal draws these inside
        // the node too, and it is what makes a blueprint readable: the value is where it is used.
        bool DrawLiteral(EditorState& state, doc::BlueprintNode& node, const doc::PinSpec& pin,
                         f32 width) {
            const doc::PinType type = pin.type;
            doc::Value value = doc::BlueprintLiteral(Ed().working, node, pin);
            const std::string key = "##" + std::to_string(node.id) + std::string(pin.name);
            bool changed = false;

            const std::vector<std::string> choices = pin.fixed ? ChoicesFor(state, node, pin)
                                                               : std::vector<std::string>{};
            if (!choices.empty()) {
                std::string current;
                if (const std::string* text = std::get_if<std::string>(&value)) current = *text;
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo(key.c_str(), current.empty() ? "self" : current.c_str())) {
                    // The component's own root, which is what a script means by "no node".
                    if (pin.name == "Node" && ImGui::Selectable("self", current.empty())) {
                        node.literals[std::string(pin.name)] = std::string();
                        changed = true;
                    }
                    for (const std::string& choice : choices)
                        if (ImGui::Selectable(choice.c_str(), choice == current)) {
                            node.literals[std::string(pin.name)] = choice;
                            changed = true;
                        }
                    ImGui::EndCombo();
                }
                return changed;
            }

            switch (type) {
                case doc::PinType::Bool: {
                    bool on = std::get_if<bool>(&value) && std::get<bool>(value);
                    if (ImGui::Checkbox(key.c_str(), &on)) {
                        node.literals[std::string(pin.name)] = on;
                        changed = true;
                    }
                    break;
                }
                case doc::PinType::Number: {
                    f32 number = std::get_if<f32>(&value) ? std::get<f32>(value) : 0.0f;
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::DragFloat(key.c_str(), &number, 0.1f, 0.0f, 0.0f, "%g")) {
                        node.literals[std::string(pin.name)] = number;
                        changed = true;
                    }
                    break;
                }
                case doc::PinType::Colour: {
                    Color colour{ 1, 1, 1, 1 };
                    if (const Color* held = std::get_if<Color>(&value)) colour = *held;
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::ColorEdit4(key.c_str(), &colour.r,
                                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                        node.literals[std::string(pin.name)] = colour;
                        changed = true;
                    }
                    break;
                }
                case doc::PinType::Text:
                case doc::PinType::Any: {
                    std::string text;
                    if (const std::string* held = std::get_if<std::string>(&value)) text = *held;
                    char buffer[256];
                    std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
                    ImGui::SetNextItemWidth(width);
                    if (ImGui::InputText(key.c_str(), buffer, sizeof(buffer))) {
                        node.literals[std::string(pin.name)] = std::string(buffer);
                        changed = true;
                    }
                    break;
                }
                case doc::PinType::Exec: break;
            }
            return changed;
        }

        // ------------------------------------------------------------------------- drawing a node

        f32 LiteralWidth(const doc::BlueprintNode& node, const doc::PinSpec& pin) {
            if (pin.type == doc::PinType::Bool || pin.type == doc::PinType::Colour) return 0.0f;
            if (pin.fixed) return 130.0f;
            (void)node;
            return pin.type == doc::PinType::Text ? 140.0f : 80.0f;
        }

        void DrawPinIcon(doc::PinType type, bool connected) {
            const ImVec4 colour = PinColour(type);
            const auto icon = type == doc::PinType::Exec ? ax::Drawing::IconType::Flow
                                                         : ax::Drawing::IconType::Circle;
            ax::Widgets::Icon(ImVec2(20.0f, 20.0f), icon, connected, colour,
                              ImVec4(colour.x, colour.y, colour.z, 0.25f));
        }

        // How wide a pin row is, so the output column can be measured before it is drawn and
        // its pins can line up on the node's right edge.
        f32 RowWidth(std::string_view label, bool icon) {
            f32 width = icon ? 22.0f : 0.0f;
            if (!label.empty())
                width += ImGui::CalcTextSize(label.data(), label.data() + label.size()).x + 6.0f;
            return width;
        }

        void DrawNode(EditorState& state, Builder& builder, doc::BlueprintNode& node,
                      const script::BlueprintHost* host, const std::string& component,
                      const std::vector<script::BlueprintProgram::Diagnostic>* diagnostics) {
            const doc::BlueprintNodeType* type = doc::FindBlueprintNodeType(node.type);
            if (!type) return;

            bool failed = false;
            std::string trouble;
            if (diagnostics)
                for (const auto& d : *diagnostics)
                    if (d.node == node.id) {
                        failed = failed || d.error;
                        if (!trouble.empty()) trouble += "\n";
                        trouble += d.message;
                    }

            if (failed) ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.9f, 0.25f, 0.25f, 1));
            builder.Begin(static_cast<ed::NodeId>(node.id));

            // A pure node has no band in Unreal — no execution passes through it, so there is no
            // moment to colour. Its name goes in the body instead, above the pins.
            const std::string title = doc::BlueprintNodeTitle(Ed().working, node);
            if (!type->pure) builder.Header(CategoryColour(type->category), title.c_str());

            const std::vector<doc::PinSpec> inputs = doc::BlueprintInputs(Ed().working, node);
            const std::vector<doc::PinSpec> outputs = doc::BlueprintOutputs(Ed().working, node);
            bool edited = false;

            if (type->pure) {
                ImGui::PushStyleColor(ImGuiCol_Text, CategoryColour(type->category).x > 0.0f
                                                     ? ImVec4(0.72f, 0.90f, 0.74f, 1.0f)
                                                     : ImVec4(1, 1, 1, 1));
                ImGui::TextUnformatted(title.c_str());
                ImGui::PopStyleColor();
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
            }

            // The outputs column, measured before anything is drawn into it.
            f32 outputsWidth = 0.0f;
            for (const doc::PinSpec& pin : outputs)
                outputsWidth = std::max(outputsWidth,
                                        RowWidth(pin.name == "Out" ? "" : pin.name, true));

            builder.BeginInputs();
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                const doc::PinSpec& pin = inputs[i];
                const bool connected = Ed().working.LinkInto(node.id, pin.name) != nullptr;
                ed::BeginPin(PinId(node.id, true, i), ed::PinKind::Input);
                ImGui::BeginGroup();
                DrawPinIcon(pin.type, connected);
                // An execution pin called In says nothing the triangle has not already said.
                if (pin.name != "In") {
                    ImGui::SameLine(0.0f, 6.0f);
                    ImGui::TextUnformatted(std::string(pin.name).c_str());
                }
                ImGui::EndGroup();
                ed::EndPin();

                // The value the pin holds when nothing is wired to it, drawn where it is used.
                if (!connected && pin.type != doc::PinType::Exec) {
                    ImGui::SameLine(0.0f, 6.0f);
                    edited |= DrawLiteral(state, node, pin, LiteralWidth(node, pin));
                }
            }
            if (inputs.empty()) ImGui::Dummy(ImVec2(1.0f, 1.0f));

            builder.NextColumn(outputsWidth);
            for (std::size_t i = 0; i < outputs.size(); ++i) {
                const doc::PinSpec& pin = outputs[i];
                const bool connected = Ed().working.LinkOutOf(node.id, pin.name) != nullptr;
                const std::string_view label = pin.name == "Out" ? std::string_view{} : pin.name;
                builder.RightAlign(RowWidth(label, true));
                ed::BeginPin(PinId(node.id, false, i), ed::PinKind::Output);
                ImGui::BeginGroup();
                if (!label.empty()) {
                    ImGui::TextUnformatted(std::string(label).c_str());
                    ImGui::SameLine(0.0f, 6.0f);
                }
                DrawPinIcon(pin.type, connected);
                ImGui::EndGroup();
                ed::EndPin();

                // What was on this pin last time the app ran through here. The half of Blueprint
                // debugging a picture of the blueprint cannot give you.
                if (host && host->Watching() && pin.type != doc::PinType::Exec) {
                    const auto& values = host->Values();
                    const auto seen = values.find(
                        script::BlueprintHost::WatchKey(component, node.id, pin.name));
                    if (seen != values.end() && !seen->second.text.empty()) {
                        ImGui::SameLine(0.0f, 6.0f);
                        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.60f, 1.0f), "%s",
                                           seen->second.text.c_str());
                    }
                }
            }
            if (outputs.empty()) ImGui::Dummy(ImVec2(1.0f, 1.0f));

            builder.End();
            if (failed) ed::PopStyleColor();

            if (edited) Commit(state, "Edit blueprint value");

            const std::string tip = !trouble.empty() ? trouble : node.comment;
            if (!tip.empty() && ed::GetHoveredNode() == static_cast<ed::NodeId>(node.id)) {
                ed::Suspend();
                ImGui::SetTooltip("%s", tip.c_str());
                ed::Resume();
            }
        }

        // ------------------------------------------------------------------------- the search

        bool Matches(std::string_view needle, std::string_view hay) {
            if (needle.empty()) return true;
            std::size_t at = 0;
            for (const char want : needle) {
                const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(want)));
                bool found = false;
                for (; at < hay.size(); ++at)
                    if (std::tolower(static_cast<unsigned char>(hay[at])) == lower) {
                        ++at;
                        found = true;
                        break;
                    }
                if (!found) return false;
            }
            return true;
        }

        // The type a dropped wire is looking for, and which side of a node it must land on.
        struct Wanted {
            bool active = false;
            doc::PinType type = doc::PinType::Exec;
            bool wantsInput = false;    // the dropped pin was an output, so the new node takes it
        };

        Wanted WantedFromDrop() {
            Wanted wanted;
            if (!Ed().dropped) return wanted;
            const u32 nodeId = PinNode(Ed().dropped);
            const doc::BlueprintNode* node = Ed().working.Find(nodeId);
            if (!node) return wanted;
            const bool input = PinIsInput(Ed().dropped);
            const std::vector<doc::PinSpec> pins = input ? doc::BlueprintInputs(Ed().working, *node)
                                                         : doc::BlueprintOutputs(Ed().working, *node);
            const std::size_t index = PinIndex(Ed().dropped);
            if (index >= pins.size()) return wanted;
            wanted.active = true;
            wanted.type = pins[index].type;
            wanted.wantsInput = !input;
            return wanted;
        }

        // Whether a node of this type has a pin a dropped wire could land on. This is what makes
        // the menu context-sensitive: pull from a number and it offers what takes a number.
        bool Fits(const doc::BlueprintNodeType& type, const Wanted& wanted) {
            if (!wanted.active) return true;
            const std::vector<doc::PinSpec>& side = wanted.wantsInput ? type.inputs : type.outputs;
            for (const doc::PinSpec& pin : side) {
                if (pin.fixed) continue;
                const bool ok = wanted.wantsInput ? doc::PinsCompatible(wanted.type, pin.type)
                                                  : doc::PinsCompatible(pin.type, wanted.type);
                if (ok) return true;
            }
            return false;
        }

        // Puts the new node down and, when a wire was dropped to summon it, joins the two.
        void Place(EditorState& state, std::string typeId, std::string target) {
            doc::BlueprintNode node;
            node.type = std::move(typeId);
            node.target = std::move(target);
            node.position = Ed().searchAt;
            const u32 id = Ed().working.AddNode(std::move(node));

            const Wanted wanted = WantedFromDrop();
            if (wanted.active) {
                const doc::BlueprintNode* placed = Ed().working.Find(id);
                const doc::BlueprintNode* from = Ed().working.Find(PinNode(Ed().dropped));
                if (placed && from) {
                    const std::vector<doc::PinSpec> side =
                        wanted.wantsInput ? doc::BlueprintInputs(Ed().working, *placed)
                                          : doc::BlueprintOutputs(Ed().working, *placed);
                    const bool droppedInput = PinIsInput(Ed().dropped);
                    const std::vector<doc::PinSpec> mine =
                        droppedInput ? doc::BlueprintInputs(Ed().working, *from)
                                     : doc::BlueprintOutputs(Ed().working, *from);
                    const std::size_t index = PinIndex(Ed().dropped);
                    for (const doc::PinSpec& pin : side) {
                        if (pin.fixed) continue;
                        const bool ok = wanted.wantsInput
                            ? doc::PinsCompatible(wanted.type, pin.type)
                            : doc::PinsCompatible(pin.type, wanted.type);
                        if (!ok) continue;
                        if (index >= mine.size()) break;
                        const std::string mineName(mine[index].name);
                        const std::string theirName(pin.name);
                        const bool exec = pin.type == doc::PinType::Exec;
                        if (wanted.wantsInput) {
                            Ed().working.DisplaceAt(from->id, mineName, false, exec);
                            Ed().working.DisplaceAt(id, theirName, true, exec);
                            Ed().working.AddLink({ 0, from->id, mineName, id, theirName });
                        } else {
                            Ed().working.DisplaceAt(id, theirName, false, exec);
                            Ed().working.DisplaceAt(from->id, mineName, true, exec);
                            Ed().working.AddLink({ 0, id, theirName, from->id, mineName });
                        }
                        break;
                    }
                }
            }

            ed::SetNodePosition(static_cast<ed::NodeId>(id),
                                ImVec2(Ed().searchAt.x, Ed().searchAt.y));
            Commit(state, "Add node");
            Ed().dropped = 0;
        }

        void DrawSearchPopup(EditorState& state) {
            if (!ImGui::BeginPopup("##blueprint-search")) { Ed().searchOpen = false; return; }

            const Wanted wanted = WantedFromDrop();
            if (wanted.active)
                ImGui::TextDisabled("nodes that take a %s", doc::PinTypeName(wanted.type));

            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(260.0f);
            ImGui::InputTextWithHint("##search", "search", Ed().search, sizeof(Ed().search));
            ImGui::Separator();

            ImGui::BeginChild("##results", ImVec2(280.0f, 380.0f));
            const std::string needle = Ed().search;

            // Variables first: they are the thing a blueprint reaches for most, and the two entries
            // per variable are what "drag a variable into the blueprint" means without a drag.
            if (!Ed().working.variables.empty()) {
                bool header = false;
                for (const doc::BlueprintVariable& variable : Ed().working.variables) {
                    for (const char* verb : { "Get ", "Set " }) {
                        const std::string label = std::string(verb) + variable.name;
                        if (!Matches(needle, label)) continue;
                        const doc::BlueprintNodeType* type =
                            doc::FindBlueprintNodeType(verb[0] == 'G' ? "var.get" : "var.set");
                        if (!type || !Fits(*type, wanted)) continue;
                        if (!header) { ImGui::SeparatorText("Variables"); header = true; }
                        if (ImGui::Selectable(label.c_str())) {
                            Place(state, verb[0] == 'G' ? "var.get" : "var.set", variable.name);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }

            for (u8 c = 0; c < static_cast<u8>(doc::BlueprintCategory::Count); ++c) {
                const auto category = static_cast<doc::BlueprintCategory>(c);
                if (category == doc::BlueprintCategory::Variable) continue;
                bool header = false;
                for (const doc::BlueprintNodeType& type : doc::BlueprintNodeTypes()) {
                    if (type.category != category) continue;
                    if (!Matches(needle, type.title) && !Matches(needle, type.summary)) continue;
                    if (!Fits(type, wanted)) continue;
                    if (!header) {
                        ImGui::SeparatorText(doc::BlueprintCategoryName(category));
                        header = true;
                    }
                    if (ImGui::Selectable(std::string(type.title).c_str())) {
                        Place(state, std::string(type.id), {});
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\n\n%s", std::string(type.summary).c_str(),
                                          std::string(type.call).c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
            return;
        }

        // ------------------------------------------------------------------------- the sidebar

        void DrawVariables(EditorState& state) {
            ImGui::SeparatorText("Variables");

            ImGui::SetNextItemWidth(-90.0f);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s", Ed().newVariable.c_str());
            if (ImGui::InputTextWithHint("##newvar", "name", buffer, sizeof(buffer)))
                Ed().newVariable = buffer;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            int type = Ed().newVariableType == doc::ValueType::Number ? 0
                     : Ed().newVariableType == doc::ValueType::Text   ? 1
                     : Ed().newVariableType == doc::ValueType::Bool   ? 2 : 3;
            if (ImGui::Combo("##newvartype", &type, "Number\0Text\0Bool\0Colour\0"))
                Ed().newVariableType = type == 0 ? doc::ValueType::Number
                                     : type == 1 ? doc::ValueType::Text
                                     : type == 2 ? doc::ValueType::Bool : doc::ValueType::Colour;

            const bool named = !Ed().newVariable.empty()
                            && !Ed().working.FindVariable(Ed().newVariable);
            ImGui::BeginDisabled(!named);
            if (ImGui::Button("Add variable", ImVec2(-1.0f, 0.0f))) {
                doc::BlueprintVariable variable;
                variable.name = Ed().newVariable;
                variable.type = Ed().newVariableType;
                switch (variable.type) {
                    case doc::ValueType::Number: variable.defaultValue = 0.0f; break;
                    case doc::ValueType::Bool:   variable.defaultValue = false; break;
                    case doc::ValueType::Colour: variable.defaultValue = Color{ 1, 1, 1, 1 }; break;
                    default: variable.defaultValue = std::string(); break;
                }
                Ed().working.SetVariable(std::move(variable));
                Ed().newVariable.clear();
                Commit(state, "Add variable");
            }
            ImGui::EndDisabled();

            for (std::size_t i = 0; i < Ed().working.variables.size(); ++i) {
                doc::BlueprintVariable& variable = Ed().working.variables[i];
                ImGui::PushID(static_cast<int>(i));
                const ImVec4 colour = PinColour(doc::PinTypeOf(variable.type));
                ImGui::TextColored(colour, "●");
                ImGui::SameLine();
                ImGui::TextUnformatted(variable.name.c_str());
                ImGui::SameLine(0.0f, 6.0f);
                if (ImGui::SmallButton("get")) {
                    Ed().searchAt = { 40.0f, 40.0f };
                    Ed().dropped = 0;
                    Place(state, "var.get", variable.name);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("set")) {
                    Ed().searchAt = { 40.0f, 40.0f };
                    Ed().dropped = 0;
                    Place(state, "var.set", variable.name);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    // Every get and set of it goes too. Leaving them would leave a blueprint that
                    // does not compile and a red node with no way to find out why.
                    const std::string name = variable.name;
                    std::erase_if(Ed().working.nodes, [&](const doc::BlueprintNode& node) {
                        return node.target == name
                            && (node.type == "var.get" || node.type == "var.set");
                    });
                    for (const doc::BlueprintNode& node : Ed().working.nodes) (void)node;
                    std::vector<u32> gone;
                    for (const doc::BlueprintLink& link : Ed().working.links)
                        if (!Ed().working.Find(link.from) || !Ed().working.Find(link.to))
                            gone.push_back(link.id);
                    for (const u32 id : gone) Ed().working.RemoveLink(id);
                    Ed().working.RemoveVariable(name);
                    Commit(state, "Remove variable");
                    ImGui::PopID();
                    break;
                }

                // The value it starts at, before anything has written to it.
                ImGui::SetNextItemWidth(-1.0f);
                bool changed = false;
                switch (variable.type) {
                    case doc::ValueType::Number: {
                        f32 number = std::get_if<f32>(&variable.defaultValue)
                                   ? std::get<f32>(variable.defaultValue) : 0.0f;
                        if (ImGui::DragFloat("##default", &number, 0.1f, 0.0f, 0.0f, "%g")) {
                            variable.defaultValue = number;
                            changed = true;
                        }
                        break;
                    }
                    case doc::ValueType::Bool: {
                        bool on = std::get_if<bool>(&variable.defaultValue)
                               && std::get<bool>(variable.defaultValue);
                        if (ImGui::Checkbox("##default", &on)) {
                            variable.defaultValue = on;
                            changed = true;
                        }
                        break;
                    }
                    case doc::ValueType::Colour: {
                        Color held{ 1, 1, 1, 1 };
                        if (const Color* value = std::get_if<Color>(&variable.defaultValue))
                            held = *value;
                        if (ImGui::ColorEdit4("##default", &held.r, ImGuiColorEditFlags_NoInputs)) {
                            variable.defaultValue = held;
                            changed = true;
                        }
                        break;
                    }
                    default: {
                        std::string text;
                        if (const std::string* value =
                                std::get_if<std::string>(&variable.defaultValue)) text = *value;
                        char field[128];
                        std::snprintf(field, sizeof(field), "%s", text.c_str());
                        if (ImGui::InputText("##default", field, sizeof(field))) {
                            variable.defaultValue = std::string(field);
                            changed = true;
                        }
                        break;
                    }
                }
                if (changed) Commit(state, "Set variable default");
                ImGui::PopID();
            }
        }

        void DrawProblems(const script::BlueprintHost* host, const std::string& component) {
            if (!host) return;
            std::vector<const script::BlueprintHost::Message*> mine;
            for (const script::BlueprintHost::Message& message : host->Messages())
                if (message.component == component) mine.push_back(&message);
            if (mine.empty()) {
                ImGui::TextColored(ImVec4(0.45f, 0.78f, 0.52f, 1.0f), "compiles");
                return;
            }
            ImGui::SeparatorText("Problems");
            ImGui::BeginChild("##problems", ImVec2(0.0f, 120.0f));
            for (const script::BlueprintHost::Message* message : mine) {
                const ImVec4 colour = message->error ? ImVec4(0.92f, 0.42f, 0.42f, 1.0f)
                                                     : ImVec4(0.92f, 0.78f, 0.40f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, colour);
                if (ImGui::Selectable(message->message.c_str())) Ed().focusNode = message->node;
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }

        // ------------------------------------------------------------------------- the canvas

        void SyncFromDocument(EditorState& state, Uuid owner) {
            const Uuid previous = Ed().owner;
            const doc::Blueprint* blueprint = state.Doc().BlueprintFor(owner);
            Ed().working = blueprint ? *blueprint : doc::Blueprint{};
            Ed().owner = owner;
            Ed().seenRevision = state.Doc().Revision();
            Ed().positioned = false;
            // Only when the blueprint being shown changed. Re-framing on every document revision would
            // yank the view out from under someone who is drawing.
            if (owner != previous) Ed().fitPending = true;
        }

        void DrawCanvas(EditorState& state, const script::BlueprintHost* host,
                        const std::string& component) {
            const script::BlueprintProgram* program = host ? host->ProgramFor(component) : nullptr;
            const std::vector<script::BlueprintProgram::Diagnostic>* diagnostics =
                program ? &program->Diagnostics() : nullptr;

            ed::SetCurrentEditor(Ed().context);
            ed::Begin("##blueprint", ImVec2(0.0f, 0.0f));

            // Where the nodes are is the document's answer, not the editor's, so it is pushed in
            // whenever the document has moved on — after a load, an undo, or an edit somewhere
            // else. Between those the editor owns them and a drag is read back below.
            if (!Ed().positioned) {
                for (const doc::BlueprintNode& node : Ed().working.nodes)
                    ed::SetNodePosition(static_cast<ed::NodeId>(node.id),
                                        ImVec2(node.position.x, node.position.y));
                for (const doc::BlueprintComment& note : Ed().working.comments) {
                    ed::SetNodePosition(static_cast<ed::NodeId>(note.id),
                                        ImVec2(note.position.x, note.position.y));
                    ed::SetGroupSize(static_cast<ed::NodeId>(note.id),
                                     ImVec2(note.size.x, note.size.y));
                }
                Ed().positioned = true;
                Ed().seeding = true;
            }

            // Regions first, so nodes draw over them.
            for (doc::BlueprintComment& note : Ed().working.comments) {
                ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.42f, 0.44f, 0.52f, 0.13f));
                ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.62f, 0.64f, 0.74f, 0.55f));
                ed::BeginNode(static_cast<ed::NodeId>(note.id));
                ImGui::PushID(static_cast<int>(note.id));
                ImGui::TextUnformatted(note.text.c_str());
                ed::Group(ImVec2(note.size.x, note.size.y));
                ImGui::PopID();
                ed::EndNode();
                ed::PopStyleColor(2);
            }

            Builder builder;
            for (doc::BlueprintNode& node : Ed().working.nodes)
                DrawNode(state, builder, node, host, component, diagnostics);

            for (const doc::BlueprintLink& link : Ed().working.links) {
                const doc::BlueprintNode* from = Ed().working.Find(link.from);
                const doc::BlueprintNode* to = Ed().working.Find(link.to);
                if (!from || !to) continue;
                const std::vector<doc::PinSpec> outputs = doc::BlueprintOutputs(Ed().working, *from);
                const std::vector<doc::PinSpec> inputs = doc::BlueprintInputs(Ed().working, *to);
                std::size_t out = 0, in = 0;
                bool foundOut = false, foundIn = false;
                for (std::size_t i = 0; i < outputs.size(); ++i)
                    if (outputs[i].name == link.fromPin) { out = i; foundOut = true; }
                for (std::size_t i = 0; i < inputs.size(); ++i)
                    if (inputs[i].name == link.toPin) { in = i; foundIn = true; }
                if (!foundOut || !foundIn) continue;
                const ImVec4 colour = PinColour(outputs[out].type);
                ed::Link(static_cast<ed::LinkId>(link.id), PinId(from->id, false, out),
                         PinId(to->id, true, in), colour,
                         outputs[out].type == doc::PinType::Exec ? 2.4f : 1.8f);
            }

            // Wires the app has just run through, drawn as Unreal draws them: the flow travels
            // along the wire, so you can watch a click go through the blueprint.
            if (host && host->Watching())
                for (const script::BlueprintHost::Flow& flow : Ed().flowThisFrame)
                    if (flow.component == component)
                        ed::Flow(static_cast<ed::LinkId>(flow.link));

            // ---- drawing new wires ------------------------------------------------------------
            if (ed::BeginCreate(ImVec4(1, 1, 1, 1), 2.0f)) {
                ed::PinId startId = 0, endId = 0;
                if (ed::QueryNewLink(&startId, &endId)) {
                    if (startId && endId) {
                        // Which end is which is up to the drag, so both orders arrive here.
                        ed::PinId outId = startId, inId = endId;
                        if (PinIsInput(outId)) std::swap(outId, inId);

                        const doc::BlueprintNode* from = Ed().working.Find(PinNode(outId));
                        const doc::BlueprintNode* to = Ed().working.Find(PinNode(inId));
                        std::string why;
                        const doc::PinSpec* outPin = nullptr;
                        const doc::PinSpec* inPin = nullptr;
                        std::vector<doc::PinSpec> outs, ins;
                        if (from && to && !PinIsInput(outId) && PinIsInput(inId)) {
                            outs = doc::BlueprintOutputs(Ed().working, *from);
                            ins  = doc::BlueprintInputs(Ed().working, *to);
                            if (PinIndex(outId) < outs.size()) outPin = &outs[PinIndex(outId)];
                            if (PinIndex(inId)  < ins.size())  inPin  = &ins[PinIndex(inId)];
                        }
                        if (!outPin || !inPin)                 why = "wire an output to an input";
                        else if (from == to)                   why = "a node cannot wire to itself";
                        else if (inPin->fixed)                 why = "that is chosen, not wired";
                        else if (!doc::PinsCompatible(outPin->type, inPin->type))
                            why = std::string("a ") + doc::PinTypeName(outPin->type)
                                + " does not fit a " + doc::PinTypeName(inPin->type);

                        if (!why.empty()) {
                            ImGui::SetTooltip("%s", why.c_str());
                            ed::RejectNewItem(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), 2.0f);
                        } else if (ed::AcceptNewItem(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), 3.0f)) {
                            const bool exec = outPin->type == doc::PinType::Exec;
                            Ed().working.DisplaceAt(from->id, std::string(outPin->name), false, exec);
                            Ed().working.DisplaceAt(to->id, std::string(inPin->name), true, exec);
                            Ed().working.AddLink({ 0, from->id, std::string(outPin->name),
                                                   to->id, std::string(inPin->name) });
                            Commit(state, "Wire");
                        }
                    }
                }

                // A wire let go over nothing: ask what should be at the other end. This is how a
                // Blueprint is drawn in practice, and it is why the search below is filtered by
                // the type that was dragged.
                ed::PinId pinId = 0;
                if (ed::QueryNewNode(&pinId)) {
                    if (ed::AcceptNewItem()) {
                        Ed().dropped = pinId;
                        const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());
                        Ed().searchAt = { at.x, at.y };
                        Ed().searchOpen = true;
                        Ed().search[0] = '\0';
                        ed::Suspend();
                        ImGui::OpenPopup("##blueprint-search");
                        ed::Resume();
                    }
                }
            }
            ed::EndCreate();

            if (ed::BeginDelete()) {
                ed::LinkId linkId = 0;
                while (ed::QueryDeletedLink(&linkId))
                    if (ed::AcceptDeletedItem()) {
                        Ed().working.RemoveLink(static_cast<u32>(linkId.Get()));
                        Commit(state, "Unwire");
                    }
                ed::NodeId nodeId = 0;
                while (ed::QueryDeletedNode(&nodeId))
                    if (ed::AcceptDeletedItem()) {
                        const u32 id = static_cast<u32>(nodeId.Get());
                        Ed().working.RemoveNode(id);
                        std::erase_if(Ed().working.comments,
                                      [&](const doc::BlueprintComment& c) { return c.id == id; });
                        Commit(state, "Delete node");
                    }
            }
            ed::EndDelete();

            // ---- context menus -------------------------------------------------------------
            ed::Suspend();
            ed::NodeId contextNode = 0;
            if (ed::ShowNodeContextMenu(&contextNode)) {
                Ed().focusNode = static_cast<u32>(contextNode.Get());
                ImGui::OpenPopup("##blueprint-node-menu");
            } else if (ed::ShowBackgroundContextMenu()) {
                const ImVec2 at = ed::ScreenToCanvas(ImGui::GetMousePos());
                Ed().searchAt = { at.x, at.y };
                Ed().dropped = 0;
                Ed().search[0] = '\0';
                Ed().searchOpen = true;
                ImGui::OpenPopup("##blueprint-search");
            }

            if (ImGui::BeginPopup("##blueprint-node-menu")) {
                doc::BlueprintNode* node = Ed().working.Find(Ed().focusNode);
                const doc::BlueprintNodeType* type = node ? doc::FindBlueprintNodeType(node->type) : nullptr;
                if (node && type) {
                    ImGui::TextDisabled("%s", std::string(type->title).c_str());
                    ImGui::Separator();
                    if (type->variadicOut && ImGui::MenuItem("Add pin")) {
                        ++node->extraPins;
                        Commit(state, "Add pin");
                    }
                    if (type->variadicOut && node->extraPins > 0 && ImGui::MenuItem("Remove pin")) {
                        const std::string gone = doc::SequencePinName(1 + node->extraPins);
                        std::erase_if(Ed().working.links, [&](const doc::BlueprintLink& link) {
                            return link.from == node->id && link.fromPin == gone;
                        });
                        --node->extraPins;
                        Commit(state, "Remove pin");
                    }
                    if (ImGui::MenuItem("Break all wires")) {
                        std::erase_if(Ed().working.links, [&](const doc::BlueprintLink& link) {
                            return link.from == node->id || link.to == node->id;
                        });
                        Commit(state, "Break wires");
                    }
                    if (ImGui::MenuItem("Delete")) {
                        Ed().working.RemoveNode(node->id);
                        Commit(state, "Delete node");
                    }
                }
                ImGui::EndPopup();
            }
            DrawSearchPopup(state);
            ed::Resume();

            ed::End();

            // Framing waits for the nodes to have been submitted once: navigating to content the
            // editor has not laid out yet navigates to nothing.
            if (Ed().fitPending && !Ed().seeding) {
                ed::NavigateToContent(0.0f);
                Ed().fitPending = false;
            }

            // A drag moved nodes; the document is told once the mouse comes up, so the whole drag
            // is one undo entry rather than one per frame of it.
            if (Ed().positioned && !Ed().seeding) {
                bool moved = false;
                for (doc::BlueprintNode& node : Ed().working.nodes) {
                    const ImVec2 at = ed::GetNodePosition(static_cast<ed::NodeId>(node.id));
                    if (at.x == FLT_MAX) continue;
                    if (at.x != node.position.x || at.y != node.position.y) {
                        node.position = { at.x, at.y };
                        moved = true;
                    }
                }
                for (doc::BlueprintComment& note : Ed().working.comments) {
                    const ImVec2 at = ed::GetNodePosition(static_cast<ed::NodeId>(note.id));
                    const ImVec2 size = ed::GetNodeSize(static_cast<ed::NodeId>(note.id));
                    if (at.x == FLT_MAX) continue;
                    if (at.x != note.position.x || at.y != note.position.y) {
                        note.position = { at.x, at.y };
                        moved = true;
                    }
                    // A size only counts when the editor has actually laid the region out. It
                    // reports a degenerate one on the frame a region is first submitted, and
                    // writing that back collapses the region — permanently, because the collapsed
                    // size is then what the next frame is seeded from.
                    const bool real = size.x > 32.0f && size.y > 32.0f;
                    if (real && (size.x != note.size.x || size.y != note.size.y)) {
                        note.size = { size.x, size.y };
                        moved = true;
                    }
                }
                if (moved) Commit(state, "Move nodes");
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) state.Commands().Break();
            }
            Ed().seeding = false;

            ed::SetCurrentEditor(nullptr);
        }

    }

    void DrawBlueprintPanel(ScriptSession& session, EditorState& state) {
        // The return value is load-bearing here, unlike in most panels. A window that is not
        // visible — a background tab, a collapsed dock — has SkipItems set, and every ImGui item
        // inside it then measures as nothing. The node editor believes those measurements and
        // stores them, so running the canvas while the tab is hidden flattens every node it
        // has not been told the size of. That is what collapsed comment regions to a line.
        if (!ImGui::Begin("Blueprint###Blueprint")) { ImGui::End(); return; }

        if (!Ed().context) {
            ed::Config config;
            // No settings file: where the nodes are is the document's business, and a .json beside
            // the binary would be a second answer that disagrees with it after every undo.
            config.SettingsFile = nullptr;
            Ed().context = ed::CreateEditor(&config);
        }

        const Uuid owner = OwnerOf(state);
        const std::string component = NameOf(state, owner);
        script::BlueprintHost* host = session.Blueprints();

        if (owner != Ed().owner || state.Doc().Revision() != Ed().seenRevision)
            SyncFromDocument(state, owner);

        // The flow the app produced since the last frame, kept for the length of this one: the
        // canvas is drawn once and the links are announced to the editor as they are drawn.
        if (host && host->Watching()) Ed().flowThisFrame = host->TakeFlow();
        else                          Ed().flowThisFrame.clear();

        // ---- toolbar -----------------------------------------------------------------------
        if (!owner.Valid()) {
            ImGui::TextDisabled("open a screen or a component to give it logic");
            ImGui::End();
            return;
        }
        ImGui::TextUnformatted(component.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.EditingComponent().Valid() ? "component" : "screen");

        ImGui::SameLine(0.0f, 16.0f);
        if (ImGui::Button("Compile")) session.Build();
        ImGui::SetItemTooltip("Ctrl+B — check every blueprint in the project");

        ImGui::SameLine();
        const bool playing = session.Playing();
        ImGui::PushStyleColor(ImGuiCol_Button, playing ? ImVec4(0.62f, 0.24f, 0.24f, 1.0f)
                                                       : ImVec4(0.20f, 0.46f, 0.30f, 1.0f));
        if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(64.0f, 0.0f))) session.Toggle();
        ImGui::PopStyleColor();

        if (host) {
            ImGui::SameLine();
            bool watching = host->Watching();
            if (ImGui::Checkbox("Watch", &watching)) host->SetWatching(watching);
            ImGui::SetItemTooltip("Show the values on the pins and the flow along the wires while "
                                  "the app runs");
        }

        ImGui::SameLine();
        if (ImGui::Button("Fit")) {
            ed::SetCurrentEditor(Ed().context);
            ed::NavigateToContent();
            ed::SetCurrentEditor(nullptr);
        }

        ImGui::SameLine();
        ImGui::TextDisabled("%zu node%s", Ed().working.nodes.size(),
                            Ed().working.nodes.size() == 1 ? "" : "s");

        ImGui::Separator();

        // ---- sidebar and canvas --------------------------------------------------------------
        ImGui::BeginChild("##blueprint-side", ImVec2(230.0f, 0.0f), ImGuiChildFlags_Borders);
        DrawVariables(state);
        DrawProblems(host, component);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##blueprint-canvas", ImVec2(0.0f, 0.0f));
        DrawCanvas(state, host, component);
        ImGui::EndChild();

        ImGui::End();
    }

}
