#pragma once

#include "vae/core/Application.h"
#include "vae/draw/Renderer.h"
#include "vae/layout/LayoutTree.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextDraw.h"

namespace vae {

    // What each laid-out node should look like when painted. At namespace scope because a nested
    // class's default member initializers cannot be used in a default argument of its enclosing
    // class.
    struct DemoVisual {
        Color fill{ 0.0f, 0.0f, 0.0f, 0.0f };
        Color border{ 0.0f, 0.0f, 0.0f, 0.0f };
        f32   borderWidth = 0.0f;
        Corners corners{};
        std::string text;
        Color textColor{ 0.92f, 0.93f, 0.96f, 1.0f };
        f32   textSize = 15.0f;
        bool  shadow = false;
        text::TextAlign align = text::TextAlign::Left;
    };

    // P5 verification: a real panel built from the layout solver, painted by the draw layer, with
    // text measured by the text layer. Nothing here hardcodes a pixel position — every rect comes
    // out of LayoutTree, so if the solver is wrong the screenshot is visibly wrong.
    class LayoutDemoLayer final : public Layer {
    public:
        LayoutDemoLayer() : Layer("LayoutDemo") {}

        void OnAttach() override;
        void OnDetach() override;
        void OnUiRender(gpu::CommandList& cmd) override;
        void OnEvent(Event& e) override;

    private:
        u32  AddNode(const layout::LayoutStyle& style, u32 parent, DemoVisual visual = {});
        void BuildTree(Vec2 viewport);
        void Paint(draw::DrawList& list, u32 node, Vec2 origin);

        draw::Renderer   m_Renderer;
        draw::DrawList   m_List;
        text::GlyphAtlas m_Atlas;
        text::TextStyle  m_Font;

        layout::LayoutTree   m_Tree;
        std::vector<DemoVisual> m_Visuals;
        u32                  m_Root = layout::LayoutTree::kInvalid;
        Vec2                 m_LastViewport{ 0.0f, 0.0f };
        bool                 m_Ready = false;
    };

}
