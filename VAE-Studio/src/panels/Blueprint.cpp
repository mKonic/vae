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
#include <map>
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
                case doc::BlueprintCategory::Collection: return { 0.14f, 0.42f, 0.36f, 1.0f };
                case doc::BlueprintCategory::Function: return { 0.34f, 0.20f, 0.46f, 1.0f };
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
                case doc::PinType::List:   return { 0.98f, 0.62f, 0.20f, 1.0f };
                case doc::PinType::Map:    return { 0.62f, 0.44f, 0.92f, 1.0f };
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
            doc::Blueprint working;             // what the editor is showing, before it is committed
            // Which canvas of it: empty for the event graph, otherwise a function's name. Only one
            // is drawn at a time, which is why the node editor can address everything by id.
            std::string function;
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
            doc::PinType newVariableType = doc::PinType::Number;
            std::string newFunction;
            bool newFunctionIsEvent = false;
            // Nodes and wires cut out of a canvas, waiting to be put down again.
            doc::BlueprintCanvas clipboard;
            std::string renaming;
            u32 focusNode = 0;                  // a node the error list asked to be shown
            // What the running app went through since the last frame. Taken once and kept for the
            // length of this one, because the canvas is drawn once and every link is announced to
            // the editor as it is drawn.
            std::vector<script::BlueprintHost::Flow> flowThisFrame;
        };

        Editing& Ed() { static Editing editing; return editing; }

        // The canvas on screen. Falls back to the event graph when the function it was showing has
        // just been deleted, which is what an undo of "add function" looks like from here.
        doc::BlueprintCanvas& Sheet() {
            if (doc::BlueprintCanvas* found = Ed().working.CanvasFor(Ed().function)) return *found;
            Ed().function.clear();
            return Ed().working.graph;
        }
        std::string_view Where() { return Ed().function; }

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
            doc::Value value = doc::BlueprintLiteral(Ed().working, node, pin, Where());
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
                // A list or a map written straight onto a pin is written the way it reads
                // back out — "a, b, c" and "k=v, k=v" — because that is the one spelling the
                // format, the state bag and the emitted C++ already agree on.
                case doc::PinType::List:
                case doc::PinType::Map:
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
            if (pin.type == doc::PinType::List || pin.type == doc::PinType::Map) return 160.0f;
            return pin.type == doc::PinType::Text ? 140.0f : 80.0f;
        }

        void DrawPinIcon(doc::PinType type, bool connected) {
            const ImVec4 colour = PinColour(type);
            // Unreal's rule: the shape says how many, the colour says of what. A grid is many
            // values in order and a square is values by name, so a list pin is still readable
            // when the node is small enough that its label is not.
            const auto icon = type == doc::PinType::Exec ? ax::Drawing::IconType::Flow
                            : type == doc::PinType::List ? ax::Drawing::IconType::Grid
                            : type == doc::PinType::Map  ? ax::Drawing::IconType::RoundSquare
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

            // Where the run is stopped reads louder than an error, because it is the one thing on
            // the canvas that is true only right now.
            const bool halted = host && host->Stopped().stopped
                             && host->Stopped().component == component
                             && host->Stopped().node == node.id;
            const bool broken = host && host->IsBreakpoint(component, node.id);
            if (halted)      ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(1.0f, 0.78f, 0.25f, 1));
            else if (failed) ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.9f, 0.25f, 0.25f, 1));
            builder.Begin(static_cast<ed::NodeId>(node.id));

            // A pure node has no band in Unreal — no execution passes through it, so there is no
            // moment to colour. Its name goes in the body instead, above the pins.
            const std::string title = doc::BlueprintNodeTitle(Ed().working, node);
            if (!type->pure) builder.Header(CategoryColour(type->category), title.c_str());

            const std::vector<doc::PinSpec> inputs = doc::BlueprintInputs(Ed().working, node, Where());
            const std::vector<doc::PinSpec> outputs = doc::BlueprintOutputs(Ed().working, node, Where());
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
                const bool connected = Sheet().LinkInto(node.id, pin.name) != nullptr;
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
                const bool connected = Sheet().LinkOutOf(node.id, pin.name) != nullptr;
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
            if (halted || failed) ed::PopStyleColor();

            // The dot Unreal puts in the corner of a node you asked it to stop at.
            if (broken || halted) {
                const ImVec2 min = ImGui::GetItemRectMin();
                if (ImDrawList* draw = ed::GetNodeBackgroundDrawList(
                        static_cast<ed::NodeId>(node.id))) {
                    draw->AddCircleFilled(ImVec2(min.x + 2.0f, min.y + 2.0f), 6.0f,
                                          halted ? IM_COL32(255, 200, 64, 255)
                                                 : IM_COL32(228, 72, 72, 255));
                }
            }

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
            const doc::BlueprintNode* node = Sheet().Find(nodeId);
            if (!node) return wanted;
            const bool input = PinIsInput(Ed().dropped);
            const std::vector<doc::PinSpec> pins = input ? doc::BlueprintInputs(Ed().working, *node, Where())
                                                         : doc::BlueprintOutputs(Ed().working, *node, Where());
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
            const u32 id = Ed().working.AddNode(Sheet(), std::move(node));

            const Wanted wanted = WantedFromDrop();
            if (wanted.active) {
                const doc::BlueprintNode* placed = Sheet().Find(id);
                const doc::BlueprintNode* from = Sheet().Find(PinNode(Ed().dropped));
                if (placed && from) {
                    const std::vector<doc::PinSpec> side =
                        wanted.wantsInput ? doc::BlueprintInputs(Ed().working, *placed, Where())
                                          : doc::BlueprintOutputs(Ed().working, *placed, Where());
                    const bool droppedInput = PinIsInput(Ed().dropped);
                    const std::vector<doc::PinSpec> mine =
                        droppedInput ? doc::BlueprintInputs(Ed().working, *from, Where())
                                     : doc::BlueprintOutputs(Ed().working, *from, Where());
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
                            Sheet().DisplaceAt(from->id, mineName, false, exec);
                            Sheet().DisplaceAt(id, theirName, true, exec);
                            Ed().working.AddLink(Sheet(), { 0, from->id, mineName, id, theirName });
                        } else {
                            Sheet().DisplaceAt(id, theirName, false, exec);
                            Sheet().DisplaceAt(from->id, mineName, true, exec);
                            Ed().working.AddLink(Sheet(), { 0, id, theirName, from->id, mineName });
                        }
                        break;
                    }
                }
            }

            // Where it lands is the document's answer, but the editor is told too so the node
            // does not appear at the origin for the one frame before the two agree. The Logic
            // panel can put a node down while the canvas is not the current editor, and the
            // canvas's own search popup can do it while it is — so save what was current.
            if (Ed().context) {
                ed::EditorContext* previous = ed::GetCurrentEditor();
                ed::SetCurrentEditor(Ed().context);
                ed::SetNodePosition(static_cast<ed::NodeId>(id),
                                    ImVec2(Ed().searchAt.x, Ed().searchAt.y));
                ed::SetCurrentEditor(previous);
            }
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

            if (!Ed().working.functions.empty()) {
                bool header = false;
                for (const doc::BlueprintFunction& function : Ed().working.functions) {
                    if (!Matches(needle, function.name)) continue;
                    const doc::BlueprintNodeType* type = doc::FindBlueprintNodeType("func.call");
                    if (!type) break;
                    // A call's pins are the signature, so whether it fits a dropped wire is a
                    // question about this function rather than about the node type.
                    if (wanted.active) {
                        bool fits = false;
                        const std::vector<doc::BlueprintParam>& side =
                            wanted.wantsInput ? function.params : function.returns;
                        for (const doc::BlueprintParam& param : side)
                            if (wanted.wantsInput ? doc::PinsCompatible(wanted.type, param.type)
                                                  : doc::PinsCompatible(param.type, wanted.type))
                                fits = true;
                        if (!function.pure && wanted.type == doc::PinType::Exec) fits = true;
                        if (!fits) continue;
                    }
                    if (!header) { ImGui::SeparatorText("Functions"); header = true; }
                    if (ImGui::Selectable(function.name.c_str())) {
                        Place(state, "func.call", function.name);
                        ImGui::CloseCurrentPopup();
                    }
                }
            }

            for (u8 c = 0; c < static_cast<u8>(doc::BlueprintCategory::Count); ++c) {
                const auto category = static_cast<doc::BlueprintCategory>(c);
                if (category == doc::BlueprintCategory::Variable) continue;
                bool header = false;
                for (const doc::BlueprintNodeType& type : doc::BlueprintNodeTypes()) {
                    if (type.category != category) continue;
                    // A function is born with its Entry and its Return; there is exactly one of
                    // each and putting a second down is not a thing anyone means.
                    if (type.id == "func.entry" || type.id == "func.return") continue;
                    // A call names what it calls, so it comes from the list of functions below
                    // rather than from the palette with nothing chosen.
                    if (type.id == "func.call") continue;
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

        // The six types a value can be, as one combo. One list rather than three, because a
        // parameter, a local and a variable are the same declaration wearing three hats.
        bool TypeCombo(const char* id, doc::PinType& type) {
            int which = type == doc::PinType::Number ? 0
                      : type == doc::PinType::Text   ? 1
                      : type == doc::PinType::Bool   ? 2
                      : type == doc::PinType::Colour ? 3
                      : type == doc::PinType::List   ? 4 : 5;
            if (!ImGui::Combo(id, &which, "Number\0Text\0Bool\0Colour\0List\0Map\0")) return false;
            type = which == 0 ? doc::PinType::Number
                 : which == 1 ? doc::PinType::Text
                 : which == 2 ? doc::PinType::Bool
                 : which == 3 ? doc::PinType::Colour
                 : which == 4 ? doc::PinType::List : doc::PinType::Map;
            return true;
        }

        doc::Value DefaultFor(doc::PinType type) { return doc::DefaultPinValue(type); }

        // The little editor for a value's starting point. Shared by variables, locals and the
        // default a parameter takes when the caller leaves its pin alone.
        bool DrawDefault(const char* id, doc::PinType type, doc::Value& value) {
            ImGui::SetNextItemWidth(-1.0f);
            switch (type) {
                case doc::PinType::Number: {
                    f32 number = std::get_if<f32>(&value) ? std::get<f32>(value) : 0.0f;
                    if (ImGui::DragFloat(id, &number, 0.1f, 0.0f, 0.0f, "%g")) {
                        value = number;
                        return true;
                    }
                    return false;
                }
                case doc::PinType::Bool: {
                    bool on = std::get_if<bool>(&value) && std::get<bool>(value);
                    if (ImGui::Checkbox(id, &on)) { value = on; return true; }
                    return false;
                }
                case doc::PinType::Colour: {
                    Color held{ 1, 1, 1, 1 };
                    if (const Color* was = std::get_if<Color>(&value)) held = *was;
                    if (ImGui::ColorEdit4(id, &held.r, ImGuiColorEditFlags_NoInputs)) {
                        value = held;
                        return true;
                    }
                    return false;
                }
                default: break;
            }
            std::string text;
            if (const std::string* was = std::get_if<std::string>(&value)) text = *was;
            char field[256];
            std::snprintf(field, sizeof(field), "%s", text.c_str());
            // A list is one item per line, which is exactly what it is on disk and in the bag.
            const bool multiline = type == doc::PinType::List || type == doc::PinType::Map;
            const bool edited = multiline
                ? ImGui::InputTextMultiline(id, field, sizeof(field), ImVec2(-1.0f, 54.0f))
                : ImGui::InputText(id, field, sizeof(field));
            if (edited) { value = std::string(field); return true; }
            return false;
        }

        // One row of a signature — a parameter, a return value or a local. Returns true when
        // something changed, and sets `remove` when the row asked to go.
        bool DrawSignatureRow(doc::BlueprintParam& param, bool& remove, bool withDefault) {
            bool changed = false;
            ImGui::PushID(&param);
            ImGui::SetNextItemWidth(-118.0f);
            char name[64];
            std::snprintf(name, sizeof(name), "%s", param.name.c_str());
            if (ImGui::InputText("##name", name, sizeof(name))) { param.name = name; changed = true; }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-24.0f);
            if (TypeCombo("##type", param.type)) {
                param.defaultValue = DefaultFor(param.type);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) remove = true;
            if (withDefault && DrawDefault("##default", param.type, param.defaultValue))
                changed = true;
            ImGui::PopID();
            return changed;
        }

        void DrawFunctions(EditorState& state) {
            ImGui::SeparatorText("Functions");

            // Which canvas is on screen. The event graph is always first, because it is the one
            // the app itself reaches.
            if (ImGui::Selectable("Event graph", Ed().function.empty())) {
                Ed().function.clear();
                Ed().positioned = false;
                Ed().fitPending = true;
            }
            for (const doc::BlueprintFunction& function : Ed().working.functions) {
                ImGui::PushID(function.name.c_str());
                const std::string label = (function.event ? "⚡ " : "ƒ ") + function.name;
                if (ImGui::Selectable(label.c_str(), Ed().function == function.name)) {
                    Ed().function = function.name;
                    Ed().positioned = false;
                    Ed().fitPending = true;
                }
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 34.0f);
                if (ImGui::SmallButton("call")) {
                    Ed().searchAt = { 40.0f, 40.0f };
                    Ed().dropped = 0;
                    Place(state, "func.call", function.name);
                }
                ImGui::PopID();
            }

            // Wide enough for the checkbox beside it and its label, measured rather than guessed:
            // a guess was four pixels short and pushed "event" off the edge of the panel.
            ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("event").x + ImGui::GetFrameHeight()
                                      + ImGui::GetStyle().ItemInnerSpacing.x
                                      + ImGui::GetStyle().ItemSpacing.x));
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s", Ed().newFunction.c_str());
            if (ImGui::InputTextWithHint("##newfn", "name", buffer, sizeof(buffer)))
                Ed().newFunction = buffer;
            ImGui::SameLine();
            ImGui::Checkbox("event", &Ed().newFunctionIsEvent);
            ImGui::SetItemTooltip("A custom event may wait on a Delay and hands nothing back. A "
                                  "function hands values back and must finish.");

            const bool named = !Ed().newFunction.empty()
                            && !Ed().working.FindFunction(Ed().newFunction);
            ImGui::BeginDisabled(!named);
            if (ImGui::Button(Ed().newFunctionIsEvent ? "Add event" : "Add function",
                              ImVec2(-1.0f, 0.0f))) {
                doc::BlueprintFunction function;
                function.name = Ed().newFunction;
                function.event = Ed().newFunctionIsEvent;
                // Every canvas needs somewhere to start, so the Entry is put down with it rather
                // than left as a thing to remember.
                doc::BlueprintNode entry;
                entry.type = "func.entry";
                entry.position = { 40.0f, 80.0f };
                Ed().working.SetFunction(std::move(function));
                doc::BlueprintFunction* added = Ed().working.FindFunction(Ed().newFunction);
                Ed().working.AddNode(added->body, std::move(entry));
                if (!added->event) {
                    doc::BlueprintNode ret;
                    ret.type = "func.return";
                    ret.position = { 420.0f, 80.0f };
                    Ed().working.AddNode(added->body, std::move(ret));
                }
                Ed().function = Ed().newFunction;
                Ed().newFunction.clear();
                Ed().positioned = false;
                Ed().fitPending = true;
                Commit(state, "Add function");
            }
            ImGui::EndDisabled();

            // The signature of the one being edited.
            doc::BlueprintFunction* editing = Ed().working.FindFunction(Ed().function);
            if (!editing) return;

            ImGui::Separator();

            // Renaming goes through the blueprint rather than the field, because every call to it
            // has to follow — a rename that leaves eight red nodes behind is not a rename.
            char rename[64];
            std::snprintf(rename, sizeof(rename), "%s", editing->name.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##rename-fn", rename, sizeof(rename))) Ed().renaming = rename;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string wanted = Ed().renaming;
                Ed().renaming.clear();
                if (!wanted.empty() && wanted != Ed().function
                    && !Ed().working.FindFunction(wanted)) {
                    Ed().working.RenameFunction(Ed().function, wanted);
                    Ed().function = wanted;
                    Commit(state, "Rename function");
                    return;
                }
            }

            ImGui::TextDisabled("%s", editing->event ? "custom event" : "function");
            if (!editing->event) {
                ImGui::SameLine();
                bool pure = editing->pure;
                if (ImGui::Checkbox("pure", &pure)) {
                    editing->pure = pure;
                    Commit(state, "Set function pure");
                    return;
                }
                ImGui::SetItemTooltip("A pure function has no execution pins and is worked out "
                                      "wherever its value is read.");
            }

            bool changed = false;
            const auto Section = [&](const char* title, std::vector<doc::BlueprintParam>& list,
                                     bool withDefault) {
                ImGui::SeparatorText(title);
                std::size_t remove = list.size();
                for (std::size_t i = 0; i < list.size(); ++i) {
                    bool gone = false;
                    if (DrawSignatureRow(list[i], gone, withDefault)) changed = true;
                    if (gone) remove = i;
                }
                if (remove < list.size()) { list.erase(list.begin() + static_cast<std::ptrdiff_t>(remove)); changed = true; }
                ImGui::PushID(title);
                if (ImGui::SmallButton("+")) {
                    list.push_back({ "value" + std::to_string(list.size()), doc::PinType::Number,
                                     0.0f });
                    changed = true;
                }
                ImGui::PopID();
            };

            Section("Takes", editing->params, true);
            if (!editing->event) Section("Hands back", editing->returns, false);

            ImGui::SeparatorText("Locals");
            std::size_t removeLocal = editing->locals.size();
            for (std::size_t i = 0; i < editing->locals.size(); ++i) {
                doc::BlueprintVariable& local = editing->locals[i];
                doc::BlueprintParam row{ local.name, local.type, local.defaultValue };
                bool gone = false;
                if (DrawSignatureRow(row, gone, true)) {
                    local = { row.name, row.type, row.defaultValue };
                    changed = true;
                }
                if (gone) removeLocal = i;
            }
            if (removeLocal < editing->locals.size()) {
                editing->locals.erase(editing->locals.begin()
                                      + static_cast<std::ptrdiff_t>(removeLocal));
                changed = true;
            }
            ImGui::BeginDisabled(editing->event);
            if (ImGui::SmallButton("+##local")) {
                editing->locals.push_back({ "local" + std::to_string(editing->locals.size()),
                                            doc::PinType::Number, 0.0f });
                changed = true;
            }
            ImGui::EndDisabled();
            if (editing->event)
                ImGui::SetItemTooltip("A custom event has no locals: it can be suspended by a "
                                      "Delay, and what a local held would not be there after.");

            if (ImGui::Button("Delete this", ImVec2(-1.0f, 0.0f))) {
                Ed().working.RemoveFunction(Ed().function);
                Ed().function.clear();
                Ed().positioned = false;
                Commit(state, "Delete function");
                return;
            }
            if (changed) Commit(state, "Edit signature");
        }

        void DrawVariables(EditorState& state) {
            ImGui::SeparatorText("Variables");

            ImGui::SetNextItemWidth(-90.0f);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s", Ed().newVariable.c_str());
            if (ImGui::InputTextWithHint("##newvar", "name", buffer, sizeof(buffer)))
                Ed().newVariable = buffer;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            TypeCombo("##newvartype", Ed().newVariableType);

            const bool named = !Ed().newVariable.empty()
                            && !Ed().working.FindVariable(Ed().newVariable);
            ImGui::BeginDisabled(!named);
            if (ImGui::Button("Add variable", ImVec2(-1.0f, 0.0f))) {
                doc::BlueprintVariable variable;
                variable.name = Ed().newVariable;
                variable.type = Ed().newVariableType;
                variable.defaultValue = DefaultFor(variable.type);
                Ed().working.SetVariable(std::move(variable));
                Ed().newVariable.clear();
                Commit(state, "Add variable");
            }
            ImGui::EndDisabled();

            for (std::size_t i = 0; i < Ed().working.variables.size(); ++i) {
                doc::BlueprintVariable& variable = Ed().working.variables[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextColored(PinColour(variable.type), "●");
                ImGui::SameLine();
                char rename[64];
                std::snprintf(rename, sizeof(rename), "%s", variable.name.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 108.0f);
                if (ImGui::InputText("##name", rename, sizeof(rename))) Ed().renaming = rename;
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const std::string wanted = Ed().renaming;
                    Ed().renaming.clear();
                    if (!wanted.empty() && wanted != variable.name
                        && !Ed().working.FindVariable(wanted)) {
                        // Through the blueprint, so every get and set of it in every canvas is
                        // renamed too.
                        Ed().working.RenameVariable(variable.name, wanted);
                        Commit(state, "Rename variable");
                        ImGui::PopID();
                        break;
                    }
                }
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
                    // Every get and set of it goes too, in every canvas. Leaving them would leave a
                    // blueprint that does not compile and a red node with no way to find out why.
                    const std::string name = variable.name;
                    const auto Prune = [&](doc::BlueprintCanvas& canvas) {
                        std::vector<u32> gone;
                        for (const doc::BlueprintNode& node : canvas.nodes)
                            if (node.target == name
                                && (node.type == "var.get" || node.type == "var.set"))
                                gone.push_back(node.id);
                        for (const u32 id : gone) canvas.RemoveNode(id);
                    };
                    Prune(Ed().working.graph);
                    for (doc::BlueprintFunction& function : Ed().working.functions)
                        Prune(function.body);
                    Ed().working.RemoveVariable(name);
                    Commit(state, "Remove variable");
                    ImGui::PopID();
                    break;
                }

                // The value it starts at, before anything has written to it.
                if (DrawDefault("##default", variable.type, variable.defaultValue))
                    Commit(state, "Set variable default");
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

        // What the editor has selected, as document ids. Only valid with the editor current,
        // which every caller of it already is.
        std::vector<u32> SelectedNodes() {
            std::vector<ed::NodeId> ids(static_cast<std::size_t>(
                std::max(0, ed::GetSelectedObjectCount())));
            const int count = ids.empty()
                ? 0 : ed::GetSelectedNodes(ids.data(), static_cast<int>(ids.size()));
            std::vector<u32> out;
            for (int i = 0; i < count; ++i) out.push_back(static_cast<u32>(ids[i].Get()));
            return out;
        }

        // The editor names this one, not the person, so it takes the first name nothing else has.
        std::string FreshFunctionName() {
            for (u32 n = 1; ; ++n) {
                const std::string name = n == 1 ? std::string("Function")
                                                : "Function " + std::to_string(n);
                if (!Ed().working.FindFunction(name)) return name;
            }
        }

        // Unreal's Ctrl+G: the selection moves into a function of its own and a call to it is left
        // where it was. The new canvas is opened, because renaming it is the next thing anyone does.
        void CollapseSelection(EditorState& state, const std::vector<u32>& selection) {
            const std::string name = FreshFunctionName();
            const doc::CollapseResult result =
                doc::CollapseIntoFunction(Ed().working, Where(), selection, name);
            if (result != doc::CollapseResult::Ok) {
                VAE_WARN("blueprint: {}", doc::CollapseResultText(result));
                return;
            }
            Commit(state, "Collapse to function");
            Ed().function = name;
            Ed().positioned = false;
            Ed().fitPending = true;
        }

        // Copying is copying the nodes and whatever wires ran between them — a wire to something
        // that was not selected is not part of what was selected.
        void CopySelection(const std::vector<ed::NodeId>& selected) {
            Ed().clipboard = {};
            std::vector<u32> ids;
            for (const ed::NodeId id : selected) ids.push_back(static_cast<u32>(id.Get()));
            for (const u32 id : ids)
                if (const doc::BlueprintNode* node = Sheet().Find(id))
                    Ed().clipboard.nodes.push_back(*node);
            for (const doc::BlueprintLink& link : Sheet().links) {
                const bool from = std::ranges::find(ids, link.from) != ids.end();
                const bool to   = std::ranges::find(ids, link.to) != ids.end();
                if (from && to) Ed().clipboard.links.push_back(link);
            }
        }

        void Paste(EditorState& state) {
            if (Ed().clipboard.nodes.empty()) return;
            // Fresh ids throughout, and the wires between them follow: pasting twice must not
            // produce two nodes claiming the same id, and a wire has to land on the copy.
            std::map<u32, u32> remap;
            for (doc::BlueprintNode node : Ed().clipboard.nodes) {
                const u32 was = node.id;
                node.id = 0;
                node.position = { node.position.x + 30.0f, node.position.y + 30.0f };
                const Vec2 at = node.position;
                remap[was] = Ed().working.AddNode(Sheet(), std::move(node));
                ed::SetNodePosition(static_cast<ed::NodeId>(remap[was]), ImVec2(at.x, at.y));
            }
            for (doc::BlueprintLink link : Ed().clipboard.links) {
                const auto from = remap.find(link.from);
                const auto to   = remap.find(link.to);
                if (from == remap.end() || to == remap.end()) continue;
                link.id = 0;
                link.from = from->second;
                link.to = to->second;
                Ed().working.AddLink(Sheet(), std::move(link));
            }
            Commit(state, "Paste");
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

        void EnsureContext() {
            if (Ed().context) return;
            ed::Config config;
            // No settings file: where the nodes are is the document's business, and a .json beside
            // the binary would be a second answer that disagrees with it after every undo.
            config.SettingsFile = nullptr;
            Ed().context = ed::CreateEditor(&config);
        }

        // The canvas and the Logic panel are two windows over one working copy, drawn in the same
        // frame in either order — whichever runs first brings it up to date and the other finds it
        // already current. Returns what is being edited, which may be nothing.
        Uuid Refresh(EditorState& state) {
            EnsureContext();
            const Uuid owner = OwnerOf(state);
            if (owner != Ed().owner || state.Doc().Revision() != Ed().seenRevision)
                SyncFromDocument(state, owner);
            return owner;
        }

        void DrawCanvas(EditorState& state, script::BlueprintHost* host,
                        const std::string& component) {
            const script::BlueprintProgram* program = host ? host->ProgramFor(component) : nullptr;
            // Only what is about the canvas on screen: a node id is unique across the blueprint,
            // but a message about a function belongs on the function.
            std::vector<script::BlueprintProgram::Diagnostic> here;
            if (program)
                for (const auto& d : program->Diagnostics())
                    if (d.function == Ed().function) here.push_back(d);
            const std::vector<script::BlueprintProgram::Diagnostic>* diagnostics = &here;

            ed::SetCurrentEditor(Ed().context);
            ed::Begin("##blueprint", ImVec2(0.0f, 0.0f));

            // Where the nodes are is the document's answer, not the editor's, so it is pushed in
            // whenever the document has moved on — after a load, an undo, or an edit somewhere
            // else. Between those the editor owns them and a drag is read back below.
            if (!Ed().positioned) {
                for (const doc::BlueprintNode& node : Sheet().nodes)
                    ed::SetNodePosition(static_cast<ed::NodeId>(node.id),
                                        ImVec2(node.position.x, node.position.y));
                for (const doc::BlueprintComment& note : Sheet().comments) {
                    ed::SetNodePosition(static_cast<ed::NodeId>(note.id),
                                        ImVec2(note.position.x, note.position.y));
                    ed::SetGroupSize(static_cast<ed::NodeId>(note.id),
                                     ImVec2(note.size.x, note.size.y));
                }
                Ed().positioned = true;
                Ed().seeding = true;
            }

            // Regions first, so nodes draw over them.
            for (doc::BlueprintComment& note : Sheet().comments) {
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
            for (doc::BlueprintNode& node : Sheet().nodes)
                DrawNode(state, builder, node, host, component, diagnostics);

            for (const doc::BlueprintLink& link : Sheet().links) {
                const doc::BlueprintNode* from = Sheet().Find(link.from);
                const doc::BlueprintNode* to = Sheet().Find(link.to);
                if (!from || !to) continue;
                const std::vector<doc::PinSpec> outputs = doc::BlueprintOutputs(Ed().working, *from, Where());
                const std::vector<doc::PinSpec> inputs = doc::BlueprintInputs(Ed().working, *to, Where());
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

                        const doc::BlueprintNode* from = Sheet().Find(PinNode(outId));
                        const doc::BlueprintNode* to = Sheet().Find(PinNode(inId));
                        std::string why;
                        const doc::PinSpec* outPin = nullptr;
                        const doc::PinSpec* inPin = nullptr;
                        std::vector<doc::PinSpec> outs, ins;
                        if (from && to && !PinIsInput(outId) && PinIsInput(inId)) {
                            outs = doc::BlueprintOutputs(Ed().working, *from, Where());
                            ins  = doc::BlueprintInputs(Ed().working, *to, Where());
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
                            Sheet().DisplaceAt(from->id, std::string(outPin->name), false, exec);
                            Sheet().DisplaceAt(to->id, std::string(inPin->name), true, exec);
                            Ed().working.AddLink(Sheet(),
                                { 0, from->id, std::string(outPin->name),
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
                        Sheet().RemoveLink(static_cast<u32>(linkId.Get()));
                        Commit(state, "Unwire");
                    }
                ed::NodeId nodeId = 0;
                while (ed::QueryDeletedNode(&nodeId))
                    if (ed::AcceptDeletedItem()) {
                        const u32 id = static_cast<u32>(nodeId.Get());
                        Sheet().RemoveNode(id);
                        std::erase_if(Sheet().comments,
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
                doc::BlueprintNode* node = Sheet().Find(Ed().focusNode);
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
                        std::erase_if(Sheet().links, [&](const doc::BlueprintLink& link) {
                            return link.from == node->id && link.fromPin == gone;
                        });
                        --node->extraPins;
                        Commit(state, "Remove pin");
                    }
                    if (host && ImGui::MenuItem(host->IsBreakpoint(component, node->id)
                                                ? "Remove breakpoint" : "Add breakpoint", "F9")) {
                        host->SetBreakpoint(component, node->id,
                                            !host->IsBreakpoint(component, node->id));
                    }
                    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
                        doc::BlueprintNode copy = *node;
                        copy.id = 0;
                        copy.position = { copy.position.x + 40.0f, copy.position.y + 40.0f };
                        const u32 made = Ed().working.AddNode(Sheet(), std::move(copy));
                        ed::SetNodePosition(static_cast<ed::NodeId>(made),
                                            ImVec2(node->position.x + 40.0f,
                                                   node->position.y + 40.0f));
                        Commit(state, "Duplicate node");
                    }
                    if (ImGui::MenuItem("Collapse to function", "Ctrl+G")) {
                        std::vector<u32> selection = SelectedNodes();
                        if (selection.empty()) selection.push_back(node->id);
                        CollapseSelection(state, selection);
                    }
                    ImGui::SetItemTooltip("Move these nodes into a function of their own and "
                                          "leave a call to it here");
                    if (ImGui::MenuItem("Break all wires")) {
                        std::erase_if(Sheet().links, [&](const doc::BlueprintLink& link) {
                            return link.from == node->id || link.to == node->id;
                        });
                        Commit(state, "Break wires");
                    }
                    if (ImGui::MenuItem("Delete")) {
                        Sheet().RemoveNode(node->id);
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
                for (doc::BlueprintNode& node : Sheet().nodes) {
                    const ImVec2 at = ed::GetNodePosition(static_cast<ed::NodeId>(node.id));
                    if (at.x == FLT_MAX) continue;
                    if (at.x != node.position.x || at.y != node.position.y) {
                        node.position = { at.x, at.y };
                        moved = true;
                    }
                }
                for (doc::BlueprintComment& note : Sheet().comments) {
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

        const Uuid owner = Refresh(state);
        const std::string component = NameOf(state, owner);
        script::BlueprintHost* host = session.Blueprints();

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
        if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(64.0f, 0.0f))) {
            session.Toggle();
            // Stopping destroys the host that was running and starting builds a new one, so the
            // pointer taken at the top of this frame is not the one the rest of it may use.
            host = session.Blueprints();
        }
        ImGui::PopStyleColor();

        // While a breakpoint holds the app, the two controls that matter are the only two on the
        // bar that do anything.
        if (host && host->Stopped().stopped) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.48f, 0.16f, 1.0f));
            if (ImGui::Button("Continue", ImVec2(78.0f, 0.0f))) host->Continue();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Step")) host->StepOver();
            ImGui::SetItemTooltip("Run one more node and stop again");
            ImGui::SameLine();
            const script::BlueprintHost::Halt& halt = host->Stopped();
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "stopped at node %u%s%s",
                               halt.node, halt.function.empty() ? "" : " in ",
                               halt.function.c_str());
        }

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
        ImGui::TextDisabled("%zu node%s", Sheet().nodes.size(),
                            Sheet().nodes.size() == 1 ? "" : "s");

        // F9 on the selection, which is where every editor puts a breakpoint, and copy/paste of
        // whatever is selected. Only while this panel has the keyboard: the canvas has its own
        // Ctrl+C and the two must not both answer.
        // Only the breakpoint needs a host; the rest are edits to the document and have to work
        // before anything has been compiled, which is when most of them are wanted.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            ed::SetCurrentEditor(Ed().context);
            std::vector<ed::NodeId> selected(static_cast<std::size_t>(
                std::max(0, ed::GetSelectedObjectCount())));
            const int count = selected.empty()
                ? 0 : ed::GetSelectedNodes(selected.data(), static_cast<int>(selected.size()));
            ed::SetCurrentEditor(nullptr);
            selected.resize(static_cast<std::size_t>(std::max(0, count)));

            if (host && ImGui::IsKeyPressed(ImGuiKey_F9, false))
                for (const ed::NodeId id : selected) {
                    const u32 node = static_cast<u32>(id.Get());
                    host->SetBreakpoint(component, node, !host->IsBreakpoint(component, node));
                }
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !selected.empty())
                CopySelection(selected);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && !selected.empty()) {
                CopySelection(selected);
                for (const ed::NodeId id : selected) Sheet().RemoveNode(static_cast<u32>(id.Get()));
                Commit(state, "Cut");
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) Paste(state);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !selected.empty()) {
                CopySelection(selected);
                Paste(state);
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false) && !selected.empty()) {
                std::vector<u32> ids;
                for (const ed::NodeId id : selected) ids.push_back(static_cast<u32>(id.Get()));
                CollapseSelection(state, ids);
            }
        }

        ImGui::Separator();

        DrawCanvas(state, host, component);

        ImGui::End();
    }

    void DrawBlueprintLogicPanel(ScriptSession& session, EditorState& state) {
        if (!ImGui::Begin("Logic###BlueprintLogic")) { ImGui::End(); return; }

        const Uuid owner = Refresh(state);
        if (!owner.Valid()) {
            ImGui::TextDisabled("open a screen or a component to give it logic");
            ImGui::End();
            return;
        }

        const std::string component = NameOf(state, owner);
        ImGui::TextUnformatted(component.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", state.EditingComponent().Valid() ? "component" : "screen");

        DrawFunctions(state);
        DrawVariables(state);
        DrawProblems(session.Blueprints(), component);

        ImGui::End();
    }

}
