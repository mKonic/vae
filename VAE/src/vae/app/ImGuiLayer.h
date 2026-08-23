#pragma once

#include "vae/core/Chrome.h"
#include "vae/gpu/Resources.h"

namespace vae::app {

    // Dear ImGui, docking enabled, on the engine's own Vulkan device.
    //
    // Split into three calls rather than one OnImGuiRender because the phases are genuinely
    // different: Begin opens the frame, Finish closes it on the CPU, and Draw records it into a
    // command list that is already inside a render pass. Everything the editor builds happens
    // between Begin and Finish.
    class ImGuiLayer final : public ChromeLayer {
    public:
        ImGuiLayer();
        ~ImGuiLayer() override;

        void OnAttach() override;
        void OnDetach() override;
        void OnEvent(Event& e) override;

        void Begin() override;
        void Finish() override;
        void Draw(gpu::CommandList& cmd) override;

        // While set, ImGui swallows input it wants and the layers below never see it. Off for a
        // scene that has to keep receiving the mouse while a panel is hovered.
        void SetBlockEvents(bool block) override { m_BlockEvents = block; }
        bool Ready() const override { return m_Ready; }

        // A texture the editor can show inside an ImGui window — the canvas, an asset thumbnail.
        // The returned handle stays valid until the texture is released.
        static u64 TextureHandle(const Ref<gpu::Texture>& texture);
        static void ReleaseTextureHandle(u64 handle);

    private:
        void ApplyTheme();
        void LoadFonts();

        bool m_Ready = false;
        bool m_BlockEvents = true;
        bool m_FrameOpen = false;
        // ImGui queues input and may hold part of it back for the NEXT frame — a press and release
        // that arrive together are deliberately split so neither is lost. An idle-driven loop that
        // renders once per event batch never gets to that second frame, and the click vanishes.
        int  m_Awake = 0;
    };

}

namespace vae::app {

    // What an editor installs with Application::SetChromeFactory. A free function rather than the
    // constructor so that naming it is the only thing that pulls ImGui into a binary.
    Scope<ChromeLayer> MakeImGuiLayer();

}
