#include "Panels.h"

#include "../Canvas.h"
#include "../ScriptSession.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdio>

namespace vae {

    namespace {

        // Nothing here survives a frame: everything the panel is about lives in the Debugger, so
        // the drawing is a view of it and never a second copy of it.
        std::string g_Filter;

        std::string Show(const doc::Value& value) {
            char buffer[128];
            switch (doc::TypeOf(value)) {
                case doc::ValueType::Unset:  return "—";
                case doc::ValueType::Bool:   return std::get<bool>(value) ? "true" : "false";
                case doc::ValueType::Number:
                    std::snprintf(buffer, sizeof buffer, "%g", std::get<f32>(value));
                    return buffer;
                case doc::ValueType::Vector2: {
                    const Vec2 v = std::get<Vec2>(value);
                    std::snprintf(buffer, sizeof buffer, "%g, %g", v.x, v.y);
                    return buffer;
                }
                case doc::ValueType::Colour: {
                    const Color c = std::get<Color>(value);
                    std::snprintf(buffer, sizeof buffer, "%.2f %.2f %.2f %.2f", c.r, c.g, c.b, c.a);
                    return buffer;
                }
                case doc::ValueType::Text:   return std::get<std::string>(value);
                case doc::ValueType::Token:  return "@" + std::get<doc::TokenRef>(value).name;
                case doc::ValueType::Bound:  return "= " + std::get<doc::Binding>(value).expression;
                default:                     return doc::ValueTypeName(doc::TypeOf(value));
            }
        }

        // Parse back into the type the value already had. A debugger that silently retypes a
        // property is worse than one that refuses the edit: the app then misbehaves for a reason
        // that is not in the code.
        bool Parse(const std::string& text, doc::Value& inout) {
            switch (doc::TypeOf(inout)) {
                case doc::ValueType::Bool:
                    inout = text == "true" || text == "1";
                    return true;
                case doc::ValueType::Number: {
                    try { inout = std::stof(text); } catch (...) { return false; }
                    return true;
                }
                case doc::ValueType::Text:
                case doc::ValueType::Unset:
                    inout = text;
                    return true;
                case doc::ValueType::Token:
                    inout = doc::TokenRef{ text.starts_with("@") ? text.substr(1) : text };
                    return true;
                default:
                    return false;
            }
        }

        bool Matches(std::string_view haystack, std::string_view needle) {
            if (needle.empty()) return true;
            const auto found = std::ranges::search(haystack, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a))
                     == std::tolower(static_cast<unsigned char>(b));
            });
            return !found.empty();
        }

        u32 RootOf(const ui::ViewTree& tree, Uuid instance) {
            for (u32 i = 0; i < tree.ViewCount(); ++i)
                if (tree.At(i).instanceId == instance) return i;
            return ui::ViewTree::kInvalid;
        }

        // --------------------------------------------------------------------------- scripts

        void DrawNodes(Debugger& debugger, const ui::ViewTree& tree,
                       const script::Runtime::LiveScript& script) {
            static const doc::Prop kPinnable[] = {
                doc::Prop::Text, doc::Prop::Value, doc::Prop::Checked, doc::Prop::SelectedIndex,
                doc::Prop::Visible, doc::Prop::Enabled, doc::Prop::Fill, doc::Prop::Open,
            };

            const u32 root = RootOf(tree, script.instance);
            if (root == ui::ViewTree::kInvalid) return;

            std::vector<u32> queue{ root };
            for (std::size_t at = 0; at < queue.size(); ++at) {
                const u32 view = queue[at];
                for (const u32 child : tree.At(view).children) queue.push_back(child);
                if (view == root || tree.At(view).name.empty()) continue;
                if (!Matches(tree.At(view).name, g_Filter)) continue;

                ImGui::PushID(static_cast<int>(view));
                ImGui::TextUnformatted(tree.At(view).name.c_str());
                for (const doc::Prop prop : kPinnable) {
                    if (!doc::IsSet(tree.ResolvedProp(view, prop))) continue;
                    ImGui::SameLine();
                    ImGui::PushID(static_cast<int>(prop));
                    if (ImGui::SmallButton(doc::PropName(prop)))
                        debugger.Add({ script.instance, script.name, tree.At(view).name,
                                       doc::PropName(prop), false });
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        }

        void DrawScripts(ScriptSession& session, Canvas& canvas) {
            Debugger& debugger = session.Debug();
            const ui::ViewTree& tree = canvas.Host().Tree();
            const auto live = session.Runtime().LiveScripts();

            if (live.empty()) {
                ImGui::TextWrapped("Nothing is mounted. A component gets a script when its name "
                                   "matches a class the module registered.");
                return;
            }

            for (const auto& script : live) {
                if (!Matches(script.component, g_Filter) && !Matches(script.name, g_Filter)) continue;

                ImGui::PushID(static_cast<int>(script.instance.Value()));
                if (ImGui::CollapsingHeader((script.name + "  ·  " + script.component).c_str(),
                                            ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Indent();

                    const auto* state = session.Runtime().StateOf(script.instance);
                    if (!state || state->empty()) {
                        ImGui::TextDisabled("no state yet");
                    } else if (ImGui::BeginTable("##state", 3, ImGuiTableFlags_SizingStretchProp
                                                               | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("key",   ImGuiTableColumnFlags_WidthStretch, 0.4f);
                        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.5f);
                        ImGui::TableSetupColumn("",      ImGuiTableColumnFlags_WidthFixed, 64.0f);
                        for (const auto& [key, value] : *state) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(key.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(Show(value).c_str());
                            ImGui::TableNextColumn();
                            ImGui::PushID(key.c_str());
                            const bool pinned = debugger.Watching(script.instance, {}, key);
                            ImGui::BeginDisabled(pinned);
                            if (ImGui::SmallButton(pinned ? "watched" : "watch"))
                                debugger.Add({ script.instance, script.name, {}, key, true });
                            ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }

                    if (script.timers) ImGui::TextDisabled("%zu timer(s) pending", script.timers);

                    ImGui::Spacing();
                    ImGui::TextDisabled("nodes — click a property to watch it");
                    DrawNodes(debugger, tree, script);
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
        }

        // --------------------------------------------------------------------------- watches

        void DrawWatches(ScriptSession& session, Canvas& canvas) {
            Debugger& debugger = session.Debug();
            if (debugger.Watches().empty()) {
                ImGui::TextWrapped("Nothing pinned. Pin a value from Scripts, then freeze it: a "
                                   "frozen value is written back after every script has run, so the "
                                   "app cannot move it — which is how you find out what keeps "
                                   "moving it.");
                return;
            }

            ui::ViewTree& tree = canvas.Host().Tree();
            if (!ImGui::BeginTable("##watch", 4, ImGuiTableFlags_SizingStretchProp
                                                 | ImGuiTableFlags_RowBg
                                                 | ImGuiTableFlags_BordersInnerH))
                return;
            ImGui::TableSetupColumn("what",   ImGuiTableColumnFlags_WidthStretch, 0.5f);
            ImGui::TableSetupColumn("value",  ImGuiTableColumnFlags_WidthStretch, 0.4f);
            ImGui::TableSetupColumn("frozen", ImGuiTableColumnFlags_WidthFixed, 56.0f);
            ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed, 26.0f);
            ImGui::TableHeadersRow();

            std::size_t remove = debugger.Watches().size();
            for (std::size_t i = 0; i < debugger.Watches().size(); ++i) {
                Debugger::Watch& watch = debugger.Watches()[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                if (watch.state) ImGui::Text("%s · state.%s", watch.owner.c_str(), watch.key.c_str());
                else             ImGui::Text("%s · %s.%s", watch.owner.c_str(),
                                             watch.node.c_str(), watch.key.c_str());

                const doc::Value current = debugger.Read(watch, session.Runtime(), tree);
                ImGui::TableNextColumn();
                if (!doc::IsSet(current) && !watch.state) {
                    ImGui::TextDisabled("gone");
                } else {
                    std::string text = Show(current);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::InputText("##value", &text, ImGuiInputTextFlags_EnterReturnsTrue)) {
                        doc::Value edited = current;
                        if (Parse(text, edited)) {
                            debugger.Write(watch, session.Runtime(), tree, edited);
                            watch.hold = edited;
                        }
                    }
                }

                ImGui::TableNextColumn();
                if (ImGui::Checkbox("##freeze", &watch.frozen) && watch.frozen)
                    watch.hold = doc::IsSet(current) ? current : watch.hold;

                ImGui::TableNextColumn();
                if (ImGui::SmallButton("x")) remove = i;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (remove < debugger.Watches().size()) debugger.Remove(remove);
        }

        // --------------------------------------------------------------------------- events

        void DrawLog(ScriptSession& session) {
            Debugger& debugger = session.Debug();

            bool paused = debugger.Paused();
            if (ImGui::Button(paused ? "Resume" : "Pause")) debugger.SetPaused(!paused);
            ImGui::SameLine();
            if (ImGui::Button("Clear")) debugger.ClearLog();
            ImGui::SameLine();
            ImGui::TextDisabled("%zu recorded", debugger.Log().size());

            ImGui::BeginChild("##events", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            const auto live = session.Runtime().LiveScripts();
            // Newest first: what just happened is what you are looking for.
            for (auto it = debugger.Log().rbegin(); it != debugger.Log().rend(); ++it) {
                if (!Matches(it->kind, g_Filter) && !Matches(it->source, g_Filter)) continue;

                std::string owner;
                for (const auto& script : live)
                    if (script.instance == it->instance) { owner = script.name; break; }

                ImGui::TextDisabled("%6llu", static_cast<unsigned long long>(it->frame));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.55f, 0.72f, 0.95f, 1.0f), "%s", it->kind.c_str());
                ImGui::SameLine(160.0f);
                ImGui::Text("%s · %s", owner.empty() ? "?" : owner.c_str(), it->source.c_str());
                if (!it->detail.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("= %s", it->detail.c_str());
                }
            }
            ImGui::EndChild();
        }

    }

    void DrawRuntimePanel(ScriptSession& session, Canvas& canvas) {
        ImGui::Begin("Runtime###Runtime");

        if (!session.Playing()) {
            ImGui::TextDisabled("Nothing is running.");
            ImGui::Spacing();
            ImGui::TextWrapped("Press F5 and this becomes the app's insides: which scripts are "
                               "mounted, what they are holding, and every event that reached them. "
                               "Pin a value to watch it, freeze it to stop the app changing it.");
            ImGui::End();
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("%.0f fps", io.Framerate);
        ImGui::SameLine();
        ImGui::TextDisabled("· %zu script(s) · %s", session.LiveInstances(), session.LanguageName());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter by name, node, component or event kind",
                                 &g_Filter);

        // Tabs, not three squeezed panes: the panel lives in a short strip at the bottom of the
        // window, and a third of that is not enough to read anything in.
        if (ImGui::BeginTabBar("##runtime")) {
            if (ImGui::BeginTabItem("Scripts")) {
                ImGui::BeginChild("##scripts");
                DrawScripts(session, canvas);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            char watches[32];
            std::snprintf(watches, sizeof watches, "Watch (%zu)###watch",
                          session.Debug().Watches().size());
            if (ImGui::BeginTabItem(watches)) {
                ImGui::BeginChild("##watches");
                DrawWatches(session, canvas);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Events")) {
                DrawLog(session);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

}
