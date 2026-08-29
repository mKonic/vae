#pragma once

#include "vae/base/Base.h"
#include "vae/base/Timestep.h"
#include "vae/core/Chrome.h"
#include "vae/core/Layer.h"
#include "vae/core/Window.h"
#include "vae/gpu/Device.h"

#include <string>
#include <vector>



namespace vae {

    struct CommandLineArgs {
        int count = 0;
        char** values = nullptr;

        std::string_view operator[](int i) const { return (i < count) ? values[i] : std::string_view{}; }
        bool Has(std::string_view flag) const;
        // `--key=value` or `--key value`; nullopt when the flag is absent or has nothing after it.
        std::optional<std::string_view> Value(std::string_view flag) const;
    };

    struct AppSpec {
        std::string name = "VAE";
        WindowSpec window{};
        CommandLineArgs args{};
        bool createWindow = true;    // false for headless tools that still want the layer stack
        bool createDevice = true;
        // Editor chrome. Off for the player and for generated apps, which ship no ImGui at all.
        bool enableImGui = false;
        Color clearColor{ 0.07f, 0.08f, 0.10f, 1.0f };
    };

    class Application {
    public:
        explicit Application(AppSpec spec);
        virtual ~Application();

        void Run();
        void Close();

        void PushLayer(Scope<Layer> layer)   { m_LayerStack.Push(std::move(layer)); }
        void PushOverlay(Scope<Layer> layer) { m_LayerStack.PushOverlay(std::move(layer)); }

        void OnEvent(Event& e);

        // The loop sleeps in WaitEvents when nothing changed. Anything that makes the next frame
        // differ from this one must say so — an animation tick, a state mutation, a resize.
        void RequestFrame() { m_FrameRequested = true; }
        void SetContinuousRendering(bool on) { m_Continuous = on; }

        Window& GetWindow() { return *m_Window; }
        // The editor chrome, or null in anything that ships. Installed by the editor before the
        // application is constructed; see Chrome.h for why it is a factory and not a type.
        ChromeLayer* Chrome() { return m_Chrome; }
        static void SetChromeFactory(ChromeFactory factory);
        gpu::Device& GetDevice() { return *m_Device; }
        bool HasDevice() const { return m_Device != nullptr; }
        // False in --selftest, --convert and every headless run, where GetWindow() would be a
        // dereferenced null.
        bool HasWindow() const { return m_Window != nullptr; }
        const AppSpec& Spec() const { return m_Spec; }
        u64 FrameCount() const { return m_FrameCount; }

        // What the process returns. A headless verification run has to be able to fail the build.
        void SetExitCode(int code) { m_ExitCode = code; }
        int  ExitCode() const { return m_ExitCode; }

        static Application& Get() { return *s_Instance; }
        // Whether there is one. A headless run — the test suite, --selftest — has none, and code
        // reached from both has to be able to ask rather than dereference null to find out.
        static bool Exists() { return s_Instance != nullptr; }

    private:
        AppSpec           m_Spec;
        Scope<Window>       m_Window;
        Scope<gpu::Device>  m_Device;
        LayerStack        m_LayerStack;
        ChromeLayer*      m_Chrome = nullptr;  // owned by the layer stack
        bool              m_Running        = true;
        bool              m_FrameRequested = true;
        bool              m_Continuous     = false;
        f64               m_LastFrameTime  = 0.0;
        u64               m_FrameCount     = 0;
        int               m_ExitCode       = 0;

        static Application* s_Instance;
    };

    // Defined by the client (Studio, Player, a generated app).
    Application* CreateApplication(CommandLineArgs args);

}
