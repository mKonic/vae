#include "Panels.h"

#include "vae/doc/Command.h"
#include "vae/doc/Serializer.h"

#include <TextEditor.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace vae {

    namespace {

        // --------------------------------------------------------------------------- colouring
        //
        // The vendored editor ships no markup language, and its bundled tokenizers are generated
        // by re2c from grammars that are not in the tree. Markup needs so little that writing it
        // by hand is smaller than adding a code generator: a tag name, an attribute name, a quoted
        // value, a comment. Everything else is punctuation and takes the default colour.
        TextEditor::Iterator TokenizeMarkup(TextEditor::Iterator start, TextEditor::Iterator end,
                                            TextEditor::Color& colour) {
            // The editor's Iterator is forward-only, so everything here walks rather than indexes.
            const auto Match = [&](TextEditor::Iterator at, const char* what) {
                for (const char* c = what; *c; ++c, ++at)
                    if (at >= end || *at != static_cast<ImWchar>(*c)) return false;
                return true;
            };
            const auto IsName = [](ImWchar c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || c == '_' || c == '-' || c == '.' || c == ':';
            };
            const auto Skip = [&](TextEditor::Iterator at, int n) {
                for (int k = 0; k < n && at < end; ++k) ++at;
                return at;
            };

            if (start >= end) return start;

            if (Match(start, "<!--")) {
                TextEditor::Iterator i = Skip(start, 4);
                while (i < end && !Match(i, "-->")) ++i;
                colour = TextEditor::Color::comment;
                return Skip(i, 3);
            }

            // A tag: `<name`, `</name`. The angle bracket travels with the name, so a tag reads as
            // one word rather than as a bracket next to an identifier.
            if (*start == '<') {
                TextEditor::Iterator i = Skip(start, 1);
                if (i < end && *i == '/') ++i;
                if (i < end && IsName(*i)) {
                    while (i < end && IsName(*i)) ++i;
                    colour = TextEditor::Color::keyword;
                    return i;
                }
                return start;
            }

            if (*start == '"') {
                TextEditor::Iterator i = Skip(start, 1);
                while (i < end && *i != '"') ++i;
                colour = TextEditor::Color::string;
                return Skip(i, 1);
            }

            // A name followed by `=` is an attribute; a bare name is body text and is left alone.
            if (IsName(*start)) {
                TextEditor::Iterator i = start;
                while (i < end && IsName(*i)) ++i;
                TextEditor::Iterator after = i;
                while (after < end && (*after == ' ' || *after == '\t')) ++after;
                colour = (after < end && *after == '=') ? TextEditor::Color::identifier
                                                        : TextEditor::Color::text;
                return i;
            }

            return start;
        }

        const TextEditor::Language* MarkupLanguage() {
            static bool built = false;
            static TextEditor::Language language;
            if (!built) {
                language.name = "VAE markup";
                language.singleLineComment = "";
                language.customTokenizer = TokenizeMarkup;
                built = true;
            }
            return &language;
        }

        // ------------------------------------------------------------------------------ state
        //
        // One panel, so one editor — the same reason the Script panel keeps one. A function-local
        // static rather than a file-scope one: the editor copies its palette from a static in its
        // own translation unit, and a file-scope object here can be constructed first, which gives
        // a fully transparent palette and an editor that draws nothing at all.
        struct Editing {
            TextEditor editor;
            Uuid node = Uuid::Invalid();   // what the buffer was loaded from
            u64  revision = 0;             // the document revision it was loaded at
            std::string error;             // what the last apply said, empty when it took
            int  errorLine = 0;
            std::size_t undoIndex = 0;     // the editor's, when the buffer was last loaded
            bool dirty = false;            // typed since it was loaded
            bool configured = false;
        };

        Editing& Edit() {
            static Editing editing;
            return editing;
        }

        void Configure(Editing& editing) {
            if (editing.configured) return;
            editing.editor.SetLanguage(MarkupLanguage());
            editing.editor.SetTabSize(2);
            editing.editor.SetShowWhitespacesEnabled(false);
            editing.editor.SetShowMatchingBrackets(true);
            editing.configured = true;
        }

        void Load(Editing& editing, EditorState& state, Uuid node) {
            editing.editor.SetText(node.Valid() ? doc::Serializer::ToXmlSubtree(state.Doc(), node)
                                                : std::string{});
            editing.node = node;
            editing.revision = state.Doc().Revision();
            editing.undoIndex = editing.editor.GetUndoIndex();
            editing.dirty = false;
            editing.error.clear();
            editing.errorLine = 0;
        }

        // The line a parse error names, as the reader spells it: "line 3: ...". Reported rather
        // than parsed out of the message, so a message that says something else still shows.
        int LineOf(const std::string& message) {
            if (!message.starts_with("line ")) return 0;
            return std::atoi(message.c_str() + 5);
        }

        void Apply(Editing& editing, EditorState& state) {
            const std::string markup = editing.editor.GetText();
            std::string error;

            // Validated against a copy first, so a refused apply is not on the undo stack at all —
            // a failed edit that still costs a Ctrl+Z is worse than no undo entry.
            doc::Document trial;
            doc::Serializer::FromXml(doc::Serializer::ToXml(state.Doc(), false, nullptr, true), trial);
            if (!doc::Serializer::FromXmlSubtree(markup, trial, editing.node, &error)) {
                editing.error = error;
                editing.errorLine = LineOf(error);
                return;
            }

            state.Execute(CreateScope<doc::ReplaceSubtreeCommand>(editing.node, markup));
            state.EndGesture();
            Load(editing, state, editing.node);
        }

    }

    void DrawMarkupPanel(EditorState& state) {
        ImGui::Begin("XML###Markup");

        Editing& editing = Edit();
        Configure(editing);

        const Uuid selected = state.Primary();
        if (!selected.Valid() || !state.Doc().Contains(selected)) {
            ImGui::TextDisabled("Select a frame to see its markup.");
            if (editing.node.Valid()) Load(editing, state, Uuid::Invalid());
            ImGui::End();
            return;
        }

        // A different selection is a different document, so it is loaded whatever was being typed.
        // The same node changed underneath — a canvas drag, an inspector edit, an undo — is only
        // reloaded when nothing is being typed here, because markup has two writers and the one
        // with a caret in it wins.
        if (selected != editing.node) {
            Load(editing, state, selected);
        } else if (!editing.dirty && state.Doc().Revision() != editing.revision) {
            Load(editing, state, selected);
        }

        const doc::Node* node = state.Doc().Find(selected);
        ImGui::TextDisabled("%s  ·  %s", node->name.c_str(), doc::NodeKindName(node->kind));

        ImGui::BeginDisabled(!editing.dirty);
        if (ImGui::Button("Apply")) Apply(editing, state);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && !editing.dirty)
            ImGui::SetTooltip("Nothing typed yet");

        ImGui::SameLine();
        ImGui::BeginDisabled(!editing.dirty);
        if (ImGui::Button("Revert")) Load(editing, state, selected);
        ImGui::EndDisabled();

        // Explicit, unlike the Script panel's continuous sync. Markup has two writers — this panel
        // and every other thing in Studio — and applying on every keystroke would rebuild the tree
        // from half-typed markup and take the selection with it.
        ImGui::SameLine();
        if (editing.dirty) ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f), "Edited");
        else               ImGui::TextDisabled("In sync with the canvas");

        ImGui::Separator();

        const bool problem = !editing.error.empty();
        const f32 footer = ImGui::GetTextLineHeightWithSpacing()
                         + (problem ? ImGui::GetTextLineHeightWithSpacing() : 0.0f);
        const f32 height = std::max(ImGui::GetContentRegionAvail().y - footer, 60.0f);

        // The font is pushed explicitly at a size. Since Dear ImGui 1.92 a font has no single size
        // of its own — glyphs are baked per requested size — and the editor asks the font to draw
        // at whatever GetFontSize() says, which is not a size it is guaranteed to have been baked
        // at. Without this the widget lays out and clips correctly and draws nothing at all.
        ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize());
        editing.editor.Render("##markup", ImVec2(0.0f, height), ImGuiChildFlags_Borders);
        ImGui::PopFont();

        // The editor's own undo index moving is what "typed since it was loaded" means — there is
        // no change flag, and comparing the whole buffer every frame to find out would be worse.
        if (editing.editor.GetUndoIndex() != editing.undoIndex) {
            editing.undoIndex = editing.editor.GetUndoIndex();
            editing.dirty = true;
            editing.error.clear();
        }

        if (problem) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.38f, 0.38f, 1.0f));
            if (ImGui::Selectable(editing.error.c_str()) && editing.errorLine > 0) {
                const auto line = static_cast<std::size_t>(editing.errorLine - 1);
                editing.editor.SetCursor(TextEditor::DocPos(line, 0));
                editing.editor.ScrollToLine(line, TextEditor::Scroll::alignMiddle);
                editing.editor.SetFocus();
            }
            ImGui::PopStyleColor();
        }

        const TextEditor::DocPos caret = editing.editor.GetCursorPosition(0);
        ImGui::TextDisabled("Ln %zu, Col %zu", caret.line + 1, caret.index + 1);

        ImGui::End();
    }

}
