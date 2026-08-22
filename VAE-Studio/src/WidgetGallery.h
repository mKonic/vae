#pragma once

#include "vae/core/Layer.h"
#include "vae/draw/Renderer.h"
#include "vae/text/GlyphAtlas.h"
#include "vae/ui/Library.h"
#include "vae/ui/UiHost.h"

namespace vae {

    // P7 verification: every widget in the standard library, in every state that has a look, laid
    // out and painted through the real pipeline. Nothing here draws a widget by hand — each one is
    // an instance of a component from ui::BuildStandardLibrary, so a screenshot is a regression
    // test for the library, the layout solver, the state overlays and the renderer at once.
    class WidgetGalleryLayer final : public Layer {
    public:
        WidgetGalleryLayer() : Layer("WidgetGallery") {}

        void OnAttach() override;
        void OnDetach() override;
        void OnUpdate(Timestep ts) override;
        void OnUiRender(gpu::CommandList& cmd) override;
        void OnEvent(Event& e) override;

    private:
        void BuildScreen();
        void ForceShowcaseStates();

        draw::Renderer   m_Renderer;
        draw::DrawList   m_List;
        text::GlyphAtlas m_Atlas;

        doc::Document m_Document;
        ui::Library   m_Library;
        ui::UiHost    m_Host;
        Uuid m_Screen = Uuid::Invalid();
        Vec2 m_Viewport{ 0.0f, 0.0f };
        f32  m_Delta = 0.0f;
        bool m_Ready = false;
    };

}
