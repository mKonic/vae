#pragma once

#include "vae/core/Layer.h"
#include "vae/doc/Serializer.h"
#include "vae/doc/Strings.h"
#include "vae/draw/Renderer.h"
#include "vae/script/Runtime.h"
#include "vae/svc/Services.h"
#include "vae/text/GlyphAtlas.h"
#include "vae/ui/AssetStore.h"
#include "vae/a11y/Bridge.h"
#include "vae/ui/UiHost.h"

#include <filesystem>
#include <string>

namespace vae::app {

    // An app, without an editor around it. The player runs one from a file; an exported build runs
    // one it constructed in code. Both are the same thing once the document exists, which is why
    // this lives in the engine rather than in either of them.
    //
    // The window opens at the size the screen was authored for and the screen is then laid out at
    // whatever the window actually is. No global scale: scaling the drawn result would magnify
    // glyphs rasterized for their design size and soften the whole app, and a resized app should
    // reflow rather than zoom.
    class RunLayer final : public Layer {
    public:
        RunLayer() : Layer("App") {}

        // Reads a document and the script beside it — the same two files the Studio saves, so
        // "run what I just designed" needs no export step. Called before the window exists, so it
        // does no GPU work: a project that cannot load must fail before a window flashes up empty.
        bool Load(const std::filesystem::path& path, std::string* error);

        // The script an exported app runs. A relative path is resolved against the executable,
        // because an exported app is moved around as a folder and must not depend on a cwd.
        void SetScript(std::filesystem::path path);

        // For a document built in code rather than read from a file. Call after filling Doc().
        bool Start();

        // Which screen to show. Empty means the first one in the document.
        void SetStartScreen(std::string name) { m_StartScreen = std::move(name); }

        // The language the app runs in. Named explicitly (--locale), or taken from the environment
        // the way every other program on the machine takes it. An app with no translation for it
        // draws what the designer authored, which is the whole point of the fallback.
        void SetLocale(std::string locale) { m_Locale = std::move(locale); }
        const std::string& Locale() const { return m_Locale; }
        // The size the window should open at: the screen's authored width and height, which are a
        // design size when the app is fluid and a hard resolution when it is not.
        Vec2 DesignSize() const { return m_DesignSize; }
        // Whether a screen's app may be resized. With no argument, the screen the app starts on —
        // which is what decides how the window is created.
        bool Resizable(Uuid screen) const;
        bool Resizable() const { return Resizable(m_Screen); }

        // Tells the app what window it has. Called from the window's own resize event in a real
        // run; public because a headless check has no window to be resized by, and what this does
        // with a fixed-resolution screen is exactly the thing worth checking.
        void Resize(Vec2 window);

        void OnAttach() override;
        void OnDetach() override;
        void OnUpdate(Timestep ts) override;
        // Rebuilds what a screen reader sees and sends it, when the screen has actually changed.
        void PublishAccessibility();
        std::string ScreenName() const;
        void OnUiRender(gpu::CommandList& cmd) override;
        void OnEvent(Event& e) override;

        // Lay out and paint one frame with no device attached. The headless check, and what the
        // export goldens compare: two documents that draw the same primitives are the same app.
        const draw::DrawList& RenderOffline(f32 dt);

        doc::Document& Doc() { return m_Document; }
        // The project's images, resolved against the folder the document was loaded from.
        ui::AssetStore& Assets() { return m_Assets; }
        svc::Services& Services() { return m_Services; }
        ui::UiHost& Host() { return m_Host; }
        script::Runtime& Runtime() { return m_Runtime; }
        const std::string& ScriptError() const { return m_ScriptError; }

    private:
        Uuid FindStartScreen() const;
        bool StartScripts();
        void Paint();
        // Asks for the next frame when something is still moving. See the definition.
        void RequestFrameIfBusy();

        // What a screen reader is allowed to do back to the app. A nested class rather than a base
        // so that "press the control at accessibility node 12" does not become part of RunLayer's
        // own surface — nothing but the bridge has any business addressing a widget that way.
        class ScreenReader final : public a11y::Actor {
        public:
            explicit ScreenReader(RunLayer& layer) : m_Layer(layer) {}
            bool Do(u32 node, a11y::Action action) override;
            bool SetCaret(u32 node, u32 start, u32 end) override;
            bool Focus(u32 node) override;
            bool EditText(u32 node, u32 start, u32 end, std::string_view insert) override;
            bool CopyText(u32 node, u32 start, u32 end, bool cut) override;
            bool PasteText(u32 node, u32 at) override;
            bool SelectChild(u32 node, u32 child, bool selected) override;

            // Everything asked for since the last frame, replayed where input belongs.
            //
            // None of it is done where it arrives. The bridge is pumped from the middle of
            // OnUpdate, and an action a widget emits after that point is thrown away by the same
            // frame's Paint — which calls ClearActions — before anything reads it. Real input
            // arrives before OnUpdate for exactly that reason, so a screen reader's does too.
            void Drain();

        private:
            struct Request {
                enum class Kind { Act, Caret, Focus, Edit, Copy, Cut, Paste };
                Kind kind = Kind::Act;
                u32  node = 0;
                u32  start = 0, end = 0;
                // What an edit puts in place of [start, end).
                std::string text;
            };

            // The field behind a node, or ViewTree::kInvalid when the node is not one, has no edit
            // state, or will not take an edit. Every editing path asks this first, so that a
            // read-only field refuses in one place rather than in four.
            u32 FieldFor(u32 node, bool forWriting) const;

            // The view behind a node, or ViewTree::kInvalid when the tree has moved on since the
            // snapshot the reader is talking about.
            u32 ViewFor(u32 node) const;
            // Queues one, and asks for the frame that will carry it out — an idle app is blocked
            // in WaitEvents and would otherwise get to it whenever the half-second net expired.
            bool Queue(Request request);

            RunLayer& m_Layer;
            std::vector<Request> m_Pending;
        };

        doc::Document m_Document;
        ui::UiHost m_Host;
        // What a screen reader sees, and what carries it there. Both null on a desktop with no
        // accessibility bus, which costs a running app nothing.
        a11y::Tree       m_Accessibility;
        Scope<a11y::Bridge> m_Bridge;
        ScreenReader     m_ScreenReader{ *this };
        u64              m_PublishedRevision = 0;
        bool             m_BridgeAsked = false;
        script::Runtime m_Runtime;
        svc::Services m_Services;
        draw::DrawList m_List;
        draw::Renderer m_Renderer;
        text::GlyphAtlas m_Atlas;
        ui::AssetStore m_Assets;
        gpu::Device* m_Device = nullptr;

        std::filesystem::path m_ScriptPath;
        // The document this run was loaded from, or empty for an exported app that built its
        // document in code. Translations sit beside one and beside the binary for the other.
        std::filesystem::path m_ProjectPath;
        std::string m_Language = "lua";
        std::string m_ScriptError;
        std::string m_StartScreen;
        std::string m_Locale;
        doc::StringTable m_Strings;

        Uuid m_Screen = Uuid::Invalid();
        Vec2 m_DesignSize{ 1280.0f, 800.0f };
        Vec2 m_WindowSize{ 0.0f, 0.0f };   // logical pixels
        Uuid m_SizedScreen = Uuid::Invalid();
        Vec2 m_SizedTo{ 0.0f, 0.0f };
        f32  m_PixelRatio = 1.0f;          // device pixels per logical pixel
    };

}
