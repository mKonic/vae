#include "vaepch.h"
#include "vae/core/Application.h"

#include "vae/base/FileSystem.h"
#include "vae/base/Version.h"
#include "vae/text/TextCache.h"

#include <chrono>

namespace vae {

    Application* Application::s_Instance = nullptr;
    namespace { ChromeFactory s_ChromeFactory = nullptr; }

    void Application::SetChromeFactory(ChromeFactory factory) { s_ChromeFactory = factory; }

    bool CommandLineArgs::Has(std::string_view flag) const {
        for (int i = 1; i < count; ++i) {
            std::string_view a = values[i];
            if (a == flag || a.starts_with(std::string(flag) + "=")) return true;
        }
        return false;
    }

    // Both spellings, because both are typed: `--bench=20` and `--bench 20`. Supporting only the
    // first is how `--convert --bench 20 file` came to read "20" as the file — the parser ignored
    // the token and the positional scan picked it up.
    std::optional<std::string_view> CommandLineArgs::Value(std::string_view flag) const {
        for (int i = 1; i < count; ++i) {
            std::string_view a = values[i];
            if (a.starts_with(flag) && a.size() > flag.size() && a[flag.size()] == '=')
                return a.substr(flag.size() + 1);
            // A bare flag takes the next token, unless there is none or it is another flag.
            if (a == flag && i + 1 < count) {
                const std::string_view next = values[i + 1];
                if (!next.starts_with("--")) return next;
            }
        }
        return std::nullopt;
    }

    namespace {
        f64 NowSeconds() {
            using clock = std::chrono::steady_clock;
            static const auto start = clock::now();
            return std::chrono::duration<f64>(clock::now() - start).count();
        }
    }

    Application::Application(AppSpec spec) : m_Spec(std::move(spec)) {
        VAE_CORE_ASSERT(!s_Instance, "an Application already exists");
        s_Instance = this;

        // First line in every log, because it is the first thing a bug report needs and the last
        // thing anyone remembers to ask for.
        VAE_CORE_INFO("VAE {} — {}", Version::String(),
                      FileSystem::EngineRoot().empty()
                          ? std::string("standalone")
                          : "engine root: " + FileSystem::EngineRoot().string());

        if (m_Spec.createWindow) {
            m_Window = Window::Create(m_Spec.window);
            if (!m_Window) {
                // Nothing below this can work, and a loop with no window never ends by itself.
                // Say so once and leave with a code a shell can test.
                VAE_CORE_ERROR("{} cannot start without a window", m_Spec.name);
                m_Running  = false;
                m_ExitCode = 3;
                return;
            }
            m_Window->SetEventCallback([this](Event& e) { OnEvent(e); });
            VAE_CORE_INFO("window '{}' created ({}x{}, scale {:.2f})",
                          m_Spec.window.title, m_Window->Width(), m_Window->Height(),
                          m_Window->ContentScale());
        }

        if (m_Spec.createDevice) {
            gpu::DeviceDesc desc;
            desc.window  = m_Window.get();
            desc.vsync   = m_Spec.window.vsync;
            desc.appName = m_Spec.name;
            m_Device = gpu::CreateDeviceWithFallback(desc);
            // With a window on screen and no device, every frame draws nothing and the user is
            // looking at a blank rectangle wondering what happened. Better to say why and stop.
            if (!m_Device) {
                VAE_CORE_ERROR("no graphics device — {} needs Vulkan 1.3", m_Spec.name);
                if (m_Window) { m_Running = false; m_ExitCode = 4; return; }
                VAE_CORE_ERROR("no graphics device — nothing will be drawn");
            }
        }

        // Pushed first so it is the bottom of the stack: OnImGuiRender runs in stack order, and the
        // dock space has to exist before any panel tries to dock into it. Built through the factory
        // the editor installed, so an app that installs none does not link one either.
        if (m_Spec.enableImGui && m_Device && s_ChromeFactory) {
            auto layer = s_ChromeFactory();
            m_Chrome = layer.get();
            m_LayerStack.Push(std::move(layer));
        }
    }

    Application::~Application() {
        // Layers own GPU resources, so they must go before the device that made them.
        if (m_Device) m_Device->WaitIdle();
        m_LayerStack.Clear();
        m_Device.reset();
        m_Window.reset();
        s_Instance = nullptr;
    }

    void Application::Close() {
        m_Running = false;
        if (m_Window) m_Window->PostEmptyEvent();   // break out of WaitEvents
    }

    void Application::OnEvent(Event& e) {
        if (e.type == EventType::WindowClose) {
            // Layers get first refusal: a document with unsaved changes has a question to ask, and
            // asking it after the window is gone is not asking it.
            for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it) {
                if (e.handled) break;
                (*it)->OnEvent(e);
            }
            if (e.handled) {
                // The window manager already set the flag; taking the close back means clearing it.
                if (m_Window) m_Window->SetShouldClose(false);
                RequestFrame();
                return;
            }
            Close();
            return;
        }

        if (e.type == EventType::WindowResize && m_Device)
            m_Device->OnWindowResize(e.size.width, e.size.height);

        // Any input at all can change what the next frame looks like.
        RequestFrame();

        for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it) {
            if (e.handled) break;
            (*it)->OnEvent(e);
        }
    }

    void Application::Run() {
        m_LastFrameTime = NowSeconds();
        // The first frame is always owed: nothing has happened yet, and a window that waits for
        // something to happen before it draws opens blank.
        m_FrameRequested = true;

        while (m_Running) {
            if (m_Window) {
                // Idle costs nothing: block until something happens rather than burning a core
                // redrawing an unchanged screen. The timeout is a safety net, not a frame clock.
                if (!m_Continuous && !m_FrameRequested) m_Window->WaitEvents(0.5);
                m_Window->OnUpdate();
                if (m_Window->ShouldClose()) { m_Running = false; break; }
            }

            const f64 now = NowSeconds();
            const Timestep ts{ static_cast<f32>(now - m_LastFrameTime) };
            m_LastFrameTime = now;

            // Waiting is not the same as not drawing, and on Wayland the difference is the whole
            // problem: the compositor's frame callback wakes WaitEvents at vsync whether or not
            // anything changed, so a loop that draws every time it wakes draws at 60 Hz forever.
            // What decides a frame is whether one was asked for.
            const bool draw = m_Continuous || m_FrameRequested;
            m_FrameRequested = false;

            const bool minimized = m_Window && m_Window->Minimized();
            if (!minimized) {
                // Layers update every pass, drawn or not: timers, network answers and script
                // clocks have to keep running, and asking for the next frame is how a layer that
                // did something says so. Cheap by construction — a layout that nothing invalidated
                // returns immediately.
                for (auto& layer : m_LayerStack) layer->OnUpdate(ts);

                if (draw && m_Device) {
                    // BeginFrame returns null when the frame must be skipped (the swapchain is
                    // being rebuilt). Recording anything in that case is undefined.
                    if (gpu::CommandList* cmd = m_Device->BeginFrame()) {
                        for (auto& layer : m_LayerStack) layer->OnRender(*cmd);

                        // The editor chrome is built before anything is recorded, so a layer that
                        // draws into a dock space already knows the rectangle it was given.
                        if (m_Chrome) {
                            m_Chrome->Begin();
                            for (auto& layer : m_LayerStack) layer->OnImGuiRender();
                            m_Chrome->Finish();
                        }

                        gpu::RenderPassDesc pass;
                        pass.clearColor = m_Spec.clearColor;
                        cmd->BeginRenderPass(pass);
                        for (auto& layer : m_LayerStack) layer->OnUiRender(*cmd);
                        if (m_Chrome) m_Chrome->Draw(*cmd);
                        cmd->EndRenderPass();

                        m_Device->EndFrame();
                        ++m_FrameCount;
                    }
                } else if (draw) {
                    ++m_FrameCount;
                }
            }

            // End of frame: shaped runs nothing asked for in a while go. Here rather than in the
            // UI layer because every app has a loop and not every app has a ViewTree.
            text::TextCache::Sweep();

            if (!m_Window) m_Running = false;   // headless: one pass, then out
        }
    }

}
