#include "Panels.h"

#include "../ScriptSession.h"

#include "vae/script/Abi.h"

#include <TextEditor.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vae {

    namespace {

        // One script per project, so one editor. It holds the text, the caret, the undo history and
        // the colouring; the session still owns the file, and the two are synced at the two moments
        // that matter — when the file changes underneath, and when the user types.
        struct Editing {
            TextEditor editor;
            TextEditor::AutoCompleteConfig completion;
            std::filesystem::path path;
            ScriptSession::Language language = ScriptSession::Language::Lua;
            std::string mirrored;          // what the session's buffer held when we last synced
            std::string markedFor;         // the diagnostics the markers were built from
            std::size_t undoIndex = 0;
            bool configured = false;
            EditorState* state = nullptr;  // for completion: the names really on the screen
        };

        // A function-local static, not a file-scope one. The editor copies its palette from a
        // static in the widget's own translation unit at construction, and a file-scope object here
        // can be constructed first — which yields a fully transparent palette and an editor that
        // lays out, scrolls and clips perfectly while drawing nothing at all.
        Editing& Edit() {
            static Editing editing;
            return editing;
        }

        // --------------------------------------------------------------------------- completion

        bool IsWordChar(char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        // A subsequence match, the way every editor's fuzzy finder works: "stx" finds "set_text".
        // Prefix matches rank first, because that is what someone typing a name in full expects.
        bool Fuzzy(std::string_view needle, std::string_view hay, int& rank) {
            std::size_t at = 0;
            std::size_t first = std::string_view::npos;
            for (const char want : needle) {
                const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(want)));
                bool found = false;
                for (; at < hay.size(); ++at) {
                    if (std::tolower(static_cast<unsigned char>(hay[at])) != lower) continue;
                    if (first == std::string_view::npos) first = at;
                    ++at;
                    found = true;
                    break;
                }
                if (!found) return false;
            }
            rank = static_cast<int>(first == std::string_view::npos ? 100 : first) * 4
                 + static_cast<int>(hay.size() / 8);
            return true;
        }

        // Every named node on the screen being edited, as the dotted path a script addresses it by.
        // This is the half a generic editor cannot have: "Card.Button.Label" is not in any keyword
        // list, it is in the document open in the next panel.
        void CollectScreenNames(EditorState& state, std::vector<std::string>& out) {
            const Uuid screen = state.ActiveScreen();
            if (!state.Doc().Contains(screen)) return;

            const auto flat = state.Doc().Flatten(screen);
            std::vector<std::string> paths(flat.size());
            for (std::size_t i = 0; i < flat.size(); ++i) {
                if (i == 0 || flat[i].name.empty()) continue;
                const u32 parent = flat[i].parent;
                const std::string& above = parent == UINT32_MAX ? paths[0] : paths[parent];
                paths[i] = above.empty() ? flat[i].name : above + "." + flat[i].name;
                out.push_back(paths[i]);
                // The leaf on its own too: most scripts address a node by its name, and only reach
                // for the path when two nodes share one.
                if (paths[i] != flat[i].name) out.push_back(flat[i].name);
            }
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }

        // The text on the caret's line, up to where the word being typed starts. This is the whole
        // of the context: what can follow `self:` is not what can follow a blank line.
        std::string LinePrefix(const std::string& text, TextEditor::DocPos start) {
            std::size_t at = 0;
            for (std::size_t line = 0; line < start.line; ++line) {
                const std::size_t next = text.find('\n', at);
                if (next == std::string::npos) return {};
                at = next + 1;
            }
            const std::size_t end = std::min(text.find('\n', at), text.size());
            return text.substr(at, std::min(start.index, end - at));
        }

        bool EndsWith(std::string_view text, std::string_view suffix) {
            return text.size() >= suffix.size()
                && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        void Suggest(TextEditor::AutoCompleteState& request) {
            if (request.inComment || request.inString) return;
            if (request.searchTerm.empty()) return;

            auto* editing = static_cast<Editing*>(request.userData);
            if (!editing) return;

            const bool lua = editing->language == ScriptSession::Language::Lua;
            const std::string text = editing->editor.GetText();
            const std::string prefix = LinePrefix(text, request.searchTermStart);

            // What is being completed is decided by what is already written. Whatever the editor
            // replaces is only the search term, so every candidate has to be insertable in its
            // place — which is why these are the bare names and not the qualified ones.
            std::vector<std::string> pool;
            bool qualified = false;
            if (lua && (EndsWith(prefix, "self:") || EndsWith(prefix, "self."))) {
                pool = script::LuaSelfMethods();
                qualified = true;
            } else if (lua && EndsWith(prefix, "event.")) {
                pool = script::LuaEventFields();
                qualified = true;
            } else if (lua && EndsWith(prefix, "vae.")) {
                pool = { "component" };
                qualified = true;
            } else if (!lua && (EndsWith(prefix, ".") || EndsWith(prefix, "->"))) {
                pool = script::CppApi();
                qualified = true;
            }

            if (!qualified) {
                pool = lua ? script::LuaGlobals() : script::CppApi();
                if (lua)
                    for (const std::string& kind : script::LuaEventKinds()) pool.push_back(kind);
                if (editing->state) CollectScreenNames(*editing->state, pool);

                // Identifiers already in the file, so a local the author just wrote completes too.
                std::string word;
                for (std::size_t i = 0; i <= text.size(); ++i) {
                    const char c = i < text.size() ? text[i] : '\0';
                    if (IsWordChar(c)) { word += c; continue; }
                    if (word.size() > 2) pool.push_back(word);
                    word.clear();
                }
            }

            std::vector<std::pair<int, std::string>> ranked;
            for (std::string& candidate : pool) {
                if (candidate == request.searchTerm) continue;
                int rank = 0;
                if (!Fuzzy(request.searchTerm, candidate, rank)) continue;
                ranked.emplace_back(rank, std::move(candidate));
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second < b.second;
            });
            ranked.erase(std::unique(ranked.begin(), ranked.end(),
                                     [](const auto& a, const auto& b) { return a.second == b.second; }),
                         ranked.end());

            request.suggestions.clear();
            for (auto& [rank, name] : ranked) {
                if (request.suggestions.size() >= 40) break;
                request.suggestions.push_back(std::move(name));
            }
        }

        // --------------------------------------------------------------------------- syncing

        const TextEditor::Language* LanguageFor(ScriptSession::Language language) {
            return language == ScriptSession::Language::Lua ? TextEditor::Language::Lua()
                                                            : TextEditor::Language::Cpp();
        }

        void Configure(Editing& editing) {
            if (editing.configured) return;
            editing.configured = true;

            TextEditor& e = editing.editor;
            e.SetTabSize(4);
            e.SetShowLineNumbersEnabled(true);
            e.SetShowMatchingBrackets(true);
            e.SetCompletePairedGlyphs(true);
            e.SetShowScrollbarMiniMapEnabled(true);
            // The widget defaults to a middot under every space. That reads as noise while you are
            // writing rather than debugging, so it is a toggle in the bar instead.
            e.SetShowWhitespacesEnabled(false);
            // The caret is drawn one line-box tall, and the box comes from the UI theme's item
            // spacing. Asking for the leading here instead keeps it proportional to the glyphs.
            e.SetLineSpacing(1.15f);

            editing.completion.triggerOnTyping = true;
            editing.completion.triggerOnShortcut = true;
            editing.completion.triggerDelay = std::chrono::milliseconds(120);
            editing.completion.callback = &Suggest;
            editing.completion.userData = &editing;
            // Set once, never per frame: SetAutoCompleteConfig copies the config *and* clears the
            // popup's active flag, so calling it every frame closes the popup as it opens.
            e.SetAutoCompleteConfig(&editing.completion);
        }

        void PushDiagnostics(Editing& editing, ScriptSession& session) {
            std::string signature = session.Output();
            for (const ScriptSession::Diagnostic& d : session.Diagnostics())
                signature += std::to_string(d.line) + ":" + d.message;
            if (signature == editing.markedFor) return;
            editing.markedFor = std::move(signature);

            editing.editor.ClearMarkers();
            for (const ScriptSession::Diagnostic& d : session.Diagnostics()) {
                const ImU32 colour = d.error ? IM_COL32(230, 97, 97, 255)
                                             : IM_COL32(242, 184, 90, 255);
                editing.editor.AddMarker(static_cast<std::size_t>(std::max(d.line - 1, 0)),
                                         colour, colour, d.message, d.message);
            }
        }

        // The session's buffer and the editor's text are two copies of one file. Whoever changed
        // last wins, and "changed" is cheap to answer on both sides: an undo index on the editor,
        // a string compare on the session's buffer, which is a few kilobytes.
        void Sync(Editing& editing, ScriptSession& session) {
            const bool movedFile = editing.path != session.SourcePath();
            const bool switchedLanguage = editing.language != session.Lang();
            if (movedFile || switchedLanguage) {
                editing.path = session.SourcePath();
                editing.language = session.Lang();
                editing.editor.SetLanguage(LanguageFor(editing.language));
            }
            if (movedFile || session.Buffer() != editing.mirrored) {
                editing.editor.SetText(session.Buffer());
                editing.mirrored = session.Buffer();
                editing.undoIndex = editing.editor.GetUndoIndex();
            }
        }

        void Pull(Editing& editing, ScriptSession& session) {
            const std::size_t undo = editing.editor.GetUndoIndex();
            if (undo == editing.undoIndex) return;
            editing.undoIndex = undo;
            editing.mirrored = editing.editor.GetText();
            session.Buffer() = editing.mirrored;
            session.MarkDirty();
        }

        // --------------------------------------------------------------------------- chrome

        void DrawToolbar(ScriptSession& session) {
            const bool playing = session.Playing();

            // Play is the one control that changes what the canvas *is*, so it leads and it is the
            // only coloured thing in the bar.
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  playing ? ImVec4(0.62f, 0.24f, 0.24f, 1.0f)
                                          : ImVec4(0.20f, 0.46f, 0.30f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  playing ? ImVec4(0.74f, 0.30f, 0.30f, 1.0f)
                                          : ImVec4(0.26f, 0.56f, 0.37f, 1.0f));
            if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(72.0f, 0.0f))) session.Toggle();
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("F5");

            ImGui::SameLine();
            if (ImGui::Button("Build")) session.Build();
            ImGui::SetItemTooltip("Ctrl+B — save and compile");

            // A project has no script until someone writes one, and most projects never need one.
            // The editor shows the template as an offer; this is what turns it into a file.
            if (!session.HasSource()) {
                ImGui::SameLine();
                if (ImGui::Button("Create script")) {
                    session.CreateSource();
                    session.Build();
                }
                ImGui::SetItemTooltip("Write this template to %s",
                                      session.SourcePath().filename().string().c_str());
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(!playing);
            if (ImGui::Button("Reload")) session.HotReload();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("F6 — swap the running code, keep the live state");

            ImGui::SameLine();
            ImGui::Dummy(ImVec2(8.0f, 0.0f));
            ImGui::SameLine();

            // Switching language switches file, so it is disabled while something is running: the
            // alternative is stopping the app out from under a click on a combo.
            ImGui::BeginDisabled(playing);
            ImGui::SetNextItemWidth(90.0f);
            int language = session.Lang() == ScriptSession::Language::Lua ? 0 : 1;
            if (ImGui::Combo("##lang", &language, "Lua\0C++\0"))
                session.SetLanguage(language == 0 ? ScriptSession::Language::Lua
                                                  : ScriptSession::Language::Cpp);
            ImGui::EndDisabled();

            ImGui::SameLine();
            bool wrap = Edit().editor.IsWordWrapEnabled();
            if (ImGui::Checkbox("Wrap", &wrap)) Edit().editor.SetWordWrapEnabled(wrap);
            ImGui::SameLine();
            bool marks = Edit().editor.IsShowWhitespacesEnabled();
            if (ImGui::Checkbox("Marks", &marks)) Edit().editor.SetShowWhitespacesEnabled(marks);
            ImGui::SetItemTooltip("Show spaces and tabs");

            ImGui::SameLine();
            const std::string name = session.SourcePath().filename().string();
            if (!session.HasSource()) {
                ImGui::TextDisabled("no script yet");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Play runs the screen either way. Creating one writes %s",
                                      session.SourcePath().string().c_str());
            } else {
                ImGui::TextDisabled("%s%s", name.c_str(), session.Dirty() ? " •" : "");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", session.SourcePath().string().c_str());
            }

            if (playing) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.45f, 0.78f, 0.52f, 1.0f), "running — %zu instance%s",
                                   session.LiveInstances(),
                                   session.LiveInstances() == 1 ? "" : "s");
            }
        }

        // The compiler's answer, one line per problem, each one a link back into the source. A wall
        // of gcc output is unreadable; a list you can click is a workflow.
        void DrawDiagnostics(ScriptSession& session, f32 height) {
            ImGui::BeginChild("##diagnostics", ImVec2(0.0f, height), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar);

            if (session.Diagnostics().empty()) {
                if (session.Output().empty())
                    ImGui::TextDisabled(!session.HasSource() ? "No script — Play runs the screen as it is."
                                        : session.Built()    ? "Built — no problems."
                                                             : "Not built yet.");
                else
                    ImGui::TextUnformatted(session.Output().c_str());
            } else {
                for (std::size_t i = 0; i < session.Diagnostics().size(); ++i) {
                    const ScriptSession::Diagnostic& d = session.Diagnostics()[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          d.error ? ImVec4(0.90f, 0.38f, 0.38f, 1.0f)
                                                  : ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
                    char label[512];
                    std::snprintf(label, sizeof label, "%d:%d  %s", d.line, d.column,
                                  d.message.c_str());
                    if (ImGui::Selectable(label)) {
                        const auto line = static_cast<std::size_t>(std::max(d.line - 1, 0));
                        Edit().editor.SetCursor(
                            TextEditor::DocPos(line, static_cast<std::size_t>(std::max(d.column - 1, 0))));
                        Edit().editor.ScrollToLine(line, TextEditor::Scroll::alignMiddle);
                        Edit().editor.SetFocus();
                    }
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }

            ImGui::EndChild();
        }

    }

    void DrawScriptPanel(ScriptSession& session, EditorState& state) {
        ImGui::Begin("Script###Script");

        Edit().state = &state;
        Configure(Edit());
        Sync(Edit(), session);
        PushDiagnostics(Edit(), session);

        // Ctrl+S means "save what I am looking at". The editor holds the keyboard while it is
        // focused, so the layer's project-save shortcut never fires here and there is no conflict.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl
            && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            Pull(Edit(), session);
            session.SaveSource();
        }

        DrawToolbar(session);
        ImGui::Separator();

        const f32 footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
        const bool problems = !session.Diagnostics().empty() || !session.Output().empty();
        const f32 diagnostics = problems ? std::min(140.0f, ImGui::GetContentRegionAvail().y * 0.4f)
                                         : 0.0f;
        const f32 height = ImGui::GetContentRegionAvail().y - footer - diagnostics
                         - (problems ? ImGui::GetStyle().ItemSpacing.y : 0.0f);

        // The font is pushed explicitly at a size. Since Dear ImGui 1.92 a font has no single
        // size of its own — glyphs are baked per requested size — and the editor asks the font to
        // draw at whatever `GetFontSize()` says, which is not a size the font is guaranteed to
        // have been baked at. Without this the widget lays out and clips correctly and draws
        // nothing at all, which looks exactly like an empty document.
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize());
        Edit().editor.Render("##src", ImVec2(0.0f, std::max(height, 60.0f)),
                             ImGuiChildFlags_Borders);
        ImGui::PopFont();
        Pull(Edit(), session);

        const TextEditor::DocPos caret = Edit().editor.GetCursorPosition(0);
        ImGui::TextDisabled("Ln %zu, Col %zu   %s   %zu bytes", caret.line + 1, caret.index + 1,
                            session.LanguageName(), session.Buffer().size());

        if (problems) DrawDiagnostics(session, diagnostics);

        ImGui::End();
    }

}
