#pragma once

// What the four selftest files share.
//
// The checks used to be one 2,500-line translation unit, which is a third of VAE-Studio in a file
// nobody could hold in their head. Splitting them by the area they exercise needed the scaffolding
// they all use to stop being file-local — so it lives here, and the counters live in Harness.cpp
// rather than being four separate copies that each report their own total.

#include "../Canvas.h"
#include "../EditorState.h"
#include "../Debugger.h"
#include "../ScriptSession.h"
#include "../StudioLayer.h"

#include "vae/app/RunLayer.h"
#include "vae/base/FileSystem.h"
#include "vae/base/Log.h"
#include "vae/base/Platform.h"
#include "vae/core/Application.h"
#include "vae/core/Input.h"
#include "vae/doc/Strings.h"
#include "vae/text/FontDB.h"

#include <imgui.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace vae::selftest {

    // Names the run the failures below belong to, and says so in the log.
    void Section(const char* name);
    // Counts, and reports a failure against the current section. Returns what it was given, so a
    // check can gate the steps that only make sense once it held.
    bool Check(bool ok, std::string_view what);
    // How many, and how many of those failed. Read once, at the end.
    int Checks();
    int Failed();
    void Reset();


    inline bool Near(f32 a, f32 b, f32 epsilon = 0.5f) { return std::abs(a - b) <= epsilon; }

    // A generous display: the canvas has to hold a 1280x800 screen at 1:1 with room around it,
    // or points the script clicks fall outside the viewport and are never hovered.
    constexpr f32 kDisplayW = 1600.0f;
    constexpr f32 kDisplayH = 1000.0f;

    // Drives the editor with no window and no GPU. ImGui's core is pure logic — hover, click,
    // drag thresholds — so a context with no backend attached reproduces input exactly, and
    // Canvas::OnImGuiRender runs the same code a mouse runs.
    class Driver {
    public:
        Driver() {
            IMGUI_CHECKVERSION();
            m_Context = ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // Claim the modern texture protocol so ImGui never falls back to asking a backend
            // that does not exist for an atlas upload.
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

            text::FontDB::Get().LoadDefaults();

            // Two frames: the first creates the canvas window, the second gives it the
            // geometry that ToScreen depends on.
            MouseTo({ kDisplayW * 0.5f, kDisplayH * 0.5f });
            Frame();
            Frame();
        }

        ~Driver() { ImGui::DestroyContext(m_Context); }

        EditorState& State()  { return m_State; }
        Canvas&      Surface() { return m_Canvas; }
        const std::vector<Rect>& GuidesDuringDrag() const { return m_DragGuides; }

        void Frame() {
            ImGui::NewFrame();
            m_Canvas.SyncScene(m_State, 1.0f / 60.0f);
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(kDisplayW, kDisplayH));
            m_Canvas.OnImGuiRender(m_State);
            ImGui::Render();
        }

        void MouseTo(Vec2 screen) { ImGui::GetIO().AddMousePosEvent(screen.x, screen.y); }
        void Button(bool down)    { ImGui::GetIO().AddMouseButtonEvent(0, down); }

        void ClickDoc(Vec2 point) {
            MouseTo(m_Canvas.ToScreen(point));
            Frame();
            Button(true);
            Frame();
            Button(false);
            Frame();
        }

        // A press, several moves and a release — the shape of a real drag, because gestures
        // that only ever see one move frame never exercise coalescing or snapping.
        void DragDoc(Vec2 from, Vec2 to, int steps = 8) {
            MouseTo(m_Canvas.ToScreen(from));
            Frame();
            Button(true);
            Frame();
            for (int i = 1; i <= steps; ++i) {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                MouseTo(m_Canvas.ToScreen(from + (to - from) * t));
                Frame();
            }
            m_DragGuides = m_Canvas.Guides();     // guides are cleared on release
            Button(false);
            Frame();
        }

    private:
        ImGuiContext* m_Context = nullptr;
        EditorState   m_State;
        Canvas        m_Canvas;
        std::vector<Rect> m_DragGuides;
    };

    inline Vec2 OffsetOf(EditorState& state, Uuid id) {
        const doc::Node* node = state.Doc().Find(id);
        return node ? node->layout.offsetStart : Vec2{ 0.0f, 0.0f };
    }

    // Places an instance and pins it to an exact box, so every later assertion is about the
    // gesture under test rather than about how wide a button's label happened to be.
    inline Uuid PlaceFixed(EditorState& state, std::string_view component, Vec2 at, Vec2 size) {
        const Uuid id = state.PlaceInstance(component, state.ActiveScreen(), at);
        if (!id.Valid()) return id;
        layout::LayoutStyle style = state.Doc().Find(id)->layout;
        style.width  = layout::Size::Px(size.x);
        style.height = layout::Size::Px(size.y);
        state.SetLayout(id, style);
        state.EndGesture();
        return id;
    }

    // A file writes an id only on a node something refers to, so anything looked up after a
    // save and a load is found the way a script finds it: by name. What has to survive a round
    // trip is the node, not the number.
    inline const doc::Node* FindByName(const doc::Document& doc, std::string_view name) {
        for (Uuid root : doc.Roots())
            for (Uuid id : doc.Subtree(root))
                if (const doc::Node* node = doc.Find(id); node && node->name == name)
                    return node;
        return nullptr;
    }

    // Keyboard shortcuts, driven through the whole editor layer rather than a copy of it.
    // They cannot be checked in a nested compositor: vc's virtual keyboard invents its own
    // keycodes, so GLFW resolves a raw Escape to a digit. Here the key events are exact.
    class Shortcuts {
    public:
        Shortcuts() {
            IMGUI_CHECKVERSION();
            m_Context = ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
            text::FontDB::Get().LoadDefaults();
            m_Layer.OnAttach();
            Frame();
            Frame();
        }
        ~Shortcuts() { ImGui::DestroyContext(m_Context); }

        StudioLayer& Layer_() { return m_Layer; }
        void Frame() {
            ImGui::NewFrame();
            m_Layer.OnImGuiRender();
            ImGui::Render();
        }

        void Press(ImGuiKey key, bool ctrl = false, bool shift = false) {
            ImGuiIO& io = ImGui::GetIO();
            if (ctrl)  { io.AddKeyEvent(ImGuiMod_Ctrl, true);  io.AddKeyEvent(ImGuiKey_LeftCtrl, true); }
            if (shift) { io.AddKeyEvent(ImGuiMod_Shift, true); io.AddKeyEvent(ImGuiKey_LeftShift, true); }
            io.AddKeyEvent(key, true);
            Frame();
            io.AddKeyEvent(key, false);
            if (ctrl)  { io.AddKeyEvent(ImGuiKey_LeftCtrl, false);  io.AddKeyEvent(ImGuiMod_Ctrl, false); }
            if (shift) { io.AddKeyEvent(ImGuiKey_LeftShift, false); io.AddKeyEvent(ImGuiMod_Shift, false); }
            Frame();
        }

    private:
        ImGuiContext* m_Context = nullptr;
        StudioLayer   m_Layer;
    };

    // A view's bounds, addressed the way a script does: a name, inside one copy of a component.
    inline Rect BoundsIn(const ui::ViewTree& tree, Uuid owner, std::string_view name) {
        u32 root = ui::ViewTree::kInvalid;
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).instanceId == owner) { root = i; break; }
        if (root == ui::ViewTree::kInvalid) return {};

        std::vector<u32> queue{ root };
        for (std::size_t at = 0; at < queue.size(); ++at) {
            const u32 view = queue[at];
            if (view != root && tree.At(view).name == name) return tree.Bounds(view);
            for (const u32 child : tree.At(view).children) queue.push_back(child);
        }
        return {};
    }

    inline std::string TextIn(const ui::ViewTree& tree, Uuid owner, std::string_view name) {
        u32 root = ui::ViewTree::kInvalid;
        for (u32 i = 0; i < tree.ViewCount(); ++i)
            if (tree.At(i).instanceId == owner) { root = i; break; }
        if (root == ui::ViewTree::kInvalid) return {};

        std::vector<u32> queue{ root };
        for (std::size_t at = 0; at < queue.size(); ++at) {
            const u32 view = queue[at];
            if (view != root && tree.At(view).name == name)
                return tree.Str(view, doc::Prop::Text);
            for (const u32 child : tree.At(view).children) queue.push_back(child);
        }
        return {};
    }

    inline Uuid InstanceNamed(const doc::Document& document, Uuid screen, std::string_view name) {
        const doc::Node* node = document.Find(screen);
        if (!node) return Uuid::Invalid();
        for (const Uuid child : node->children)
            if (const doc::Node* c = document.Find(child); c && c->name == name) return child;
        return Uuid::Invalid();
    }

    // --- the checks, one file per area ---------------------------------------------------------
    void RunDirect();      // Direct.cpp    — what the mouse does to a document
    void RunAuthoring();   // Authoring.cpp — what the panels author
    void RunProjects();    // Projects.cpp  — a project on disk
    void RunRunning();     // Running.cpp   — play, and the tools that watch it

}
