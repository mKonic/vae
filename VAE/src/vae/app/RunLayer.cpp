#include "vaepch.h"
#include "vae/app/RunLayer.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Platform.h"
#include "vae/base/Log.h"
#include "vae/core/Application.h"
#include "vae/script/LuaHost.h"
#include "vae/script/NativeHost.h"
#include "vae/text/FontDB.h"

#include <algorithm>

namespace vae::app {

    bool RunLayer::Load(const std::filesystem::path& path, std::string* error) {
        if (!doc::Serializer::Load(path, m_Document, error)) return false;

        // The script sits beside the document and is named after it, exactly as the Studio writes
        // it. A native module wins when both are present: a project that ships a compiled module
        // has already been built and should not be recompiled on someone else's machine.
        std::filesystem::path base = path;
        base.replace_extension();
        const std::filesystem::path native =
            std::filesystem::path(base).concat(platform::ModuleExtension());
        const std::filesystem::path lua    = std::filesystem::path(base).concat(".lua");
        if (std::filesystem::exists(native))   { m_ScriptPath = native; m_Language = "cpp"; }
        else if (std::filesystem::exists(lua)) { m_ScriptPath = lua;    m_Language = "lua"; }

        // The app's own folder is its sandbox and its saved state lives in it. Beside the project
        // rather than in a hidden directory: an exported app is a folder you can move, and its
        // saved state moving with it is the behaviour that surprises nobody.
        m_Services.FileSystem().AddRoot(path.parent_path());
        m_Services.Store().Open(std::filesystem::path(base).concat(".store.json"));
        // Images are relative to the project folder, so an app that is moved keeps its pictures.
        m_Assets.SetRoot(path.parent_path());
        m_Assets.Rebind(m_Document);

        if (!Start()) {
            if (error) *error = m_Document.Roots().empty()
                              ? path.string() + " is empty"
                              : path.string() + " has no screens to show";
            return false;
        }

        VAE_INFO("app: {} · screen '{}' · {}x{} · {}", path.filename().string(),
                 m_Document.Find(m_Screen)->name, m_DesignSize.x, m_DesignSize.y,
                 m_ScriptPath.empty() ? std::string("no script")
                                      : m_ScriptPath.filename().string());
        return true;
    }

    void RunLayer::SetScript(std::filesystem::path path) {
        if (path.empty()) { m_ScriptPath.clear(); return; }
        m_ScriptPath = path.is_absolute() ? std::move(path)
                                          : FileSystem::ExecutableDir() / std::move(path);
        m_Language = m_ScriptPath.extension() == platform::ModuleExtension() ? "cpp" : "lua";
        if (!std::filesystem::exists(m_ScriptPath)) {
            VAE_WARN("app: no script at {} — the screens will not do anything",
                     m_ScriptPath.string());
            m_ScriptPath.clear();
        }
    }

    bool RunLayer::Start() {
        m_Screen = FindStartScreen();
        if (!m_Screen.Valid()) return false;

        // A document built in code is an exported app, and an exported app is a folder you move:
        // its pictures sit beside the binary at the same relative paths the project used. Load()
        // has already pointed the store at the project folder, so this only fills the other case.
        if (m_Assets.Root().empty()) m_Assets.SetRoot(FileSystem::ExecutableDir());
        m_Assets.Rebind(m_Document);

        if (const doc::Node* screen = m_Document.Find(m_Screen))
            m_DesignSize = { screen->layout.width.value, screen->layout.height.value };
        if (m_DesignSize.x < 1.0f || m_DesignSize.y < 1.0f) m_DesignSize = { 1280.0f, 800.0f };
        return true;
    }

    Uuid RunLayer::FindStartScreen() const {
        // A name on the command line wins, because it is what someone asked for right now. Failing
        // that, the screen the designer marked as the start; failing that, the first one there is.
        if (!m_StartScreen.empty()) {
            for (const Uuid screen : m_Document.Screens())
                if (const doc::Node* node = m_Document.Find(screen); node
                    && node->name == m_StartScreen)
                    return screen;
            VAE_WARN("app: no screen called '{}'", m_StartScreen);
        }
        return m_Document.StartScreen();
    }

    bool RunLayer::StartScripts() {
        m_Runtime.Attach(m_Host, m_Document);
        m_Runtime.SetServices(&m_Services);
        if (m_ScriptPath.empty()) return true;

        Scope<script::Host> host;
        if (m_Language == "cpp") host = CreateScope<script::NativeHost>();
        else                     host = CreateScope<script::LuaHost>();
        host->Bind(m_Runtime.Api());

        if (!host->Load(m_ScriptPath, &m_ScriptError)) {
            // The app still runs. A broken script means a screen that does nothing, which is far
            // easier to diagnose than an app that refuses to start and says why in a log file.
            VAE_ERROR("app: {}", m_ScriptError);
            return false;
        }
        m_Runtime.AddHost(std::move(host));
        return true;
    }

    void RunLayer::OnAttach() {
        text::FontDB::Get().LoadDefaults();
        if (!m_Screen.Valid()) Start();
        m_Host.SetDocument(m_Document, m_Screen);
        StartScripts();

        Application& app = Application::Get();
        if (!app.HasDevice()) return;

        m_Device = &app.GetDevice();
        if (!m_Renderer.Init(*m_Device, m_Device->GetSwapchain()->ColorFormat()))
            VAE_ERROR("app: renderer failed to initialise");
        m_Atlas.Init(*m_Device);
        m_Assets.SetDevice(m_Device);
        app.SetContinuousRendering(true);
    }

    void RunLayer::OnDetach() {
        m_Services.Net().CancelAll();
        m_Services.Store().Flush();
        m_Runtime.Detach();
        m_Runtime.ClearHosts();
        m_Atlas.Shutdown();
        m_Renderer.Shutdown();
    }

    // The window IS the screen. Not a letterbox around a fixed image: laying the screen out at the
    // window's own size keeps every pixel 1:1 and lets Fill and Percent do what they were written
    // for — an app resized is an app relaid out, not zoomed.
    bool RunLayer::Resizable(Uuid screen) const {
        const doc::Node* node = m_Document.Find(screen);
        // Default true, and deliberately so: an app that does not fit the window it was given is
        // the exception, and the exception is the one that should have to say so.
        return !node || node->props.Flag(doc::Prop::Resizable, true);
    }

    void RunLayer::Resize(Vec2 window) {
        if (window.x < 1.0f || window.y < 1.0f) return;
        m_WindowSize = window;

        // Whichever page is showing, not the one the app started on. Navigating replaces the host's
        // root, and a screen that kept its authored size after a navigation is an app that fills the
        // window until you go somewhere and then does not.
        const Uuid showing = m_Host.CurrentScreen().Valid() ? m_Host.CurrentScreen() : m_Screen;
        if (showing == m_SizedScreen && window == m_SizedTo) return;

        // Only pages. A dialog keeps the size it was designed at — that is what makes it a dialog
        // rather than another full-screen page.
        if (doc::IsOverlayKind(m_Document.KindOf(showing))) return;

        // A screen the designer pinned to a resolution keeps it. The window is created at that size
        // and cannot be dragged, so this should never be reached for one — but a compositor that
        // ignores the hint, or a tiling WM, will send a resize anyway, and stretching the design to
        // fit it is precisely what "fixed resolution" was asked not to do.
        if (!Resizable(showing)) return;

        if (doc::Node* screen = m_Document.Find(showing)) {
            screen->layout.width  = layout::Size::Px(window.x);
            screen->layout.height = layout::Size::Px(window.y);
            m_Document.Touch(showing);
            m_SizedScreen = showing;
            m_SizedTo = window;
        }
    }

    void RunLayer::OnUpdate(Timestep ts) {
        Application& app = Application::Get();
        if (app.HasDevice()) {
            // Window::Width is a framebuffer size — device pixels. Layout works in logical pixels,
            // so the screen is that many device pixels divided by the display's scale, and the
            // difference is made up at paint time rather than in the document.
            m_PixelRatio = std::max(app.GetWindow().ContentScale(), 0.25f);
            Resize({ static_cast<f32>(app.GetWindow().Width()) / m_PixelRatio,
                     static_cast<f32>(app.GetWindow().Height()) / m_PixelRatio });
        }

        // The order in a frame, and each step needs the one before it:
        //   deliver — scripts see what the widgets produced, on the screen it happened on
        //   navigate — declared destinations take effect, now that the click has been seen
        //   lay out — the tree that is showing after that navigation
        //   sync — mount what appeared and unmount what left
        //   tick — timers and on_update, for what is live now
        m_Runtime.Dispatch(m_Host.TakeActions());
        if (m_Host.ApplyNavigation()) Resize(m_WindowSize);
        m_Host.Update(m_WindowSize, ts);
        m_Runtime.Sync();
        m_Runtime.Update(ts);
        // A script can navigate too, and its new screen needs the window just as much.
        if (m_Host.CurrentScreen() != m_SizedScreen) {
            Resize(m_WindowSize);
            m_Host.Update(m_WindowSize, 0.0f);
        }
        // Last: an answer from the network is delivered here, so a script sees it in the frame
        // after it arrived rather than the frame after that.
        m_Services.Tick(ts);
    }

    void RunLayer::Paint() {
        m_List.Reset();
        // One uniform transform from logical units to device pixels. Glyphs are already rasterized
        // at device resolution and emitted at logical size, so this scales the geometry and leaves
        // the type exactly as sharp as it was drawn.
        const bool scaled = m_PixelRatio != 1.0f;
        if (scaled) m_List.PushTransform({ m_PixelRatio, m_PixelRatio }, { 0.0f, 0.0f });

        ui::PaintContext paint;
        paint.list = &m_List;
        paint.atlas = m_Device ? &m_Atlas : nullptr;
        paint.assets = &m_Assets;
        paint.pixelRatio = m_PixelRatio;
        m_Host.Paint(paint);
        m_Host.ClearActions();

        if (scaled) m_List.PopTransform();
    }

    void RunLayer::OnUiRender(gpu::CommandList& cmd) {
        if (!m_Device) return;
        Paint();
        m_Renderer.NewFrame();
        m_Renderer.Render(cmd, m_List, m_WindowSize * m_PixelRatio);
    }

    const draw::DrawList& RunLayer::RenderOffline(f32 dt) {
        if (m_WindowSize.x < 1.0f) Resize(m_DesignSize);
        m_Runtime.Dispatch(m_Host.TakeActions());
        if (m_Host.ApplyNavigation()) Resize(m_WindowSize);
        m_Host.Update(m_WindowSize, dt);
        m_Runtime.Sync();
        m_Runtime.Update(dt);
        m_Services.Tick(dt);
        Paint();
        return m_List;
    }

    void RunLayer::OnEvent(Event& e) {
        if (e.type == EventType::WindowResize) {
            Resize({ static_cast<f32>(e.size.width) / m_PixelRatio,
                     static_cast<f32>(e.size.height) / m_PixelRatio });
            return;
        }
        if (e.type == EventType::WindowScaleChanged) {
            // Dragged onto a display with a different scale. Nothing about the design changes; the
            // atlas is asked for bigger glyphs and the geometry transform picks up the rest.
            m_PixelRatio = std::max(e.scale.x, 0.25f);
            return;
        }

        // Pointer coordinates arrive in device pixels and the widgets think in logical ones.
        Event forwarded = e;
        switch (e.type) {
            case EventType::MouseMoved:
                forwarded.mouse = { e.mouse.x / m_PixelRatio, e.mouse.y / m_PixelRatio };
                break;
            case EventType::MouseButtonPressed:
            case EventType::MouseButtonReleased:
                forwarded.button = { e.button.button, e.button.x / m_PixelRatio,
                                     e.button.y / m_PixelRatio };
                break;
            default: break;
        }

        // Escape and the mouse's back button are what "back" means to whoever pressed them, and
        // neither belongs to a widget. The host tries the top overlay, then the screen stack.
        if (e.type == EventType::KeyPressed && e.key.code == Key::Escape && m_Host.GoBack()) {
            e.handled = true;
            return;
        }

        if (m_Host.Dispatch(forwarded)) e.handled = true;
        if (m_Device)
            Application::Get().GetWindow().SetCursor(static_cast<Cursor>(m_Host.Cursor()));
    }

}
