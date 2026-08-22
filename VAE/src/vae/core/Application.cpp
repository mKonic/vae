#include "vaepch.h"
#include "vae/core/Application.h"

#include "vae/app/ImGuiLayer.h"
#include "vae/base/FileSystem.h"

#include <chrono>

namespace vae {

    Application* Application::s_Instance = nullptr;

    bool CommandLineArgs::Has(std::string_view flag) const {
        for (int i = 1; i < count; ++i) {
            std::string_view a = values[i];
            if (a == flag || a.starts_with(std::string(flag) + "=")) return true;
        }
        return false;
    }

    std::optional<std::string_view> CommandLineArgs::Value(std::string_view flag) const {
        for (int i = 1; i < count; ++i) {
            std::string_view a = values[i];
            if (a.starts_with(flag) && a.size() > flag.size() && a[flag.size()] == '=')
                return a.substr(flag.size() + 1);
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

        VAE_CORE_INFO("VAE engine root: {}", FileSystem::EngineRoot().string());

        if (m_Spec.createWindow) {
            m_Window = Window::Create(m_Spec.window);
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
            if (!m_Device) VAE_CORE_ERROR("no graphics device — nothing will be drawn");
        }

        // Pushed first so it is the bottom of the stack: OnImGuiRender runs in stack order, and the
        // dock space has to exist before any panel tries to dock into it.
        if (m_Spec.enableImGui && m_Device) {
            auto layer = CreateScope<app::ImGuiLayer>();
            m_ImGui = layer.get();
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

            m_FrameRequested = false;

            const bool minimized = m_Window && m_Window->Minimized();
            if (!minimized) {
                for (auto& layer : m_LayerStack) layer->OnUpdate(ts);

                if (m_Device) {
                    // BeginFrame returns null when the frame must be skipped (the swapchain is
                    // being rebuilt). Recording anything in that case is undefined.
                    if (gpu::CommandList* cmd = m_Device->BeginFrame()) {
                        for (auto& layer : m_LayerStack) layer->OnRender(*cmd);

                        // The editor chrome is built before anything is recorded, so a layer that
                        // draws into a dock space already knows the rectangle it was given.
                        if (m_ImGui) {
                            m_ImGui->Begin();
                            for (auto& layer : m_LayerStack) layer->OnImGuiRender();
                            m_ImGui->Finish();
                        }

                        gpu::RenderPassDesc pass;
                        pass.clearColor = m_Spec.clearColor;
                        cmd->BeginRenderPass(pass);
                        for (auto& layer : m_LayerStack) layer->OnUiRender(*cmd);
                        if (m_ImGui) m_ImGui->Draw(*cmd);
                        cmd->EndRenderPass();

                        m_Device->EndFrame();
                        ++m_FrameCount;
                    }
                } else {
                    ++m_FrameCount;
                }
            }

            if (!m_Window) m_Running = false;   // headless: one pass, then out
        }
    }

}
