#pragma once

#include "../EditorState.h"

#include <filesystem>

namespace vae {

    class Canvas;
    class ScriptSession;

    // Panels are free functions over the editor state rather than objects: they own no state worth
    // keeping between frames, and ImGui already remembers everything that is really per-panel.
    void DrawLayersPanel(EditorState& state);
    void DrawInspectorPanel(EditorState& state);
    void DrawComponentsPanel(EditorState& state, Canvas& canvas);
    void DrawScreensPanel(EditorState& state, Canvas& canvas);
    // Takes the canvas because the images live on it: the store owns the textures, and the
    // panel is the one place that says why one is not showing.
    void DrawAssetsPanel(EditorState& state, Canvas& canvas);
    // The project's palette: add, recolour, rename and delete tokens.
    void DrawTokensPanel(EditorState& state);
    void DrawConsolePanel();
    // Takes the editor state as well as the session: completion that knows the names on
    // the screen being edited is the half a generic code editor cannot have.
    void DrawScriptPanel(ScriptSession& session, EditorState& state);
    // The selection as markup: the same XML the file holds, scoped to one node. Explicit Apply
    // rather than the Script panel's continuous sync, because this is a second writer for a model
    // the canvas and the Inspector are already writing.
    void DrawMarkupPanel(EditorState& state);
    // The app's insides while it is running: what is mounted, what it holds, what reached it.
    void DrawRuntimePanel(ScriptSession& session, Canvas& canvas);
    // The project's files. Returns a project to open when one was clicked, empty otherwise — the
    // panel does not own the document, and opening one is the layer's business.
    std::filesystem::path DrawFilesPanel(ScriptSession& session, EditorState& state);

    // Installs the log sink the Console panel reads. Safe to call more than once.
    void InitConsolePanel();

}
