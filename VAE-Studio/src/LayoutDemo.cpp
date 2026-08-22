#include "LayoutDemo.h"

namespace vae {

    using namespace vae::layout;
    using namespace vae::draw;

    namespace {
        constexpr Color kAccent{ 0.93f, 0.62f, 0.27f, 1.0f };
        constexpr Color kPanel { 0.13f, 0.14f, 0.17f, 1.0f };
        constexpr Color kSunken{ 0.09f, 0.10f, 0.12f, 1.0f };
        constexpr Color kEdge  { 0.24f, 0.26f, 0.31f, 1.0f };
        constexpr Color kText  { 0.92f, 0.93f, 0.96f, 1.0f };
        constexpr Color kMuted { 0.58f, 0.61f, 0.68f, 1.0f };

        LayoutStyle Row(f32 gap = 0.0f, Edges padding = {}) {
            LayoutStyle s;
            s.mode = LayoutMode::Stack;
            s.axis = Axis::Row;
            s.gap = gap;
            s.padding = padding;
            s.align = Align::Center;
            return s;
        }

        LayoutStyle Column(f32 gap = 0.0f, Edges padding = {}) {
            LayoutStyle s;
            s.mode = LayoutMode::Stack;
            s.axis = Axis::Column;
            s.gap = gap;
            s.padding = padding;
            return s;
        }
    }

    void LayoutDemoLayer::OnAttach() {
        auto& app = Application::Get();
        if (!app.HasDevice()) return;
        auto& device = app.GetDevice();

        const gpu::Format format = device.GetSwapchain() ? device.GetSwapchain()->ColorFormat()
                                                         : gpu::Format::BGRA8_UNORM;
        if (!m_Renderer.Init(device, format)) return;
        m_Atlas.Init(device);

        auto& fonts = text::FontDB::Get();
        fonts.LoadDefaults();
        m_Font = fonts.Style({ "", text::FontWeight::Regular, text::FontSlant::Normal, 15.0f });
        m_Ready = true;
    }

    void LayoutDemoLayer::OnDetach() {
        if (!m_Ready) return;
        m_Atlas.Shutdown();
        m_Renderer.Shutdown();
    }

    u32 LayoutDemoLayer::AddNode(const LayoutStyle& style, u32 parent, DemoVisual visual) {
        const u32 index = m_Tree.Add(style, parent);
        if (m_Visuals.size() <= index) m_Visuals.resize(index + 1);

        // Text nodes get their intrinsic size from the text layer, which is exactly the seam that
        // lets a label-sized button hug its label.
        if (!visual.text.empty()) {
            auto style2 = m_Font;
            style2.size = visual.textSize;
            m_Tree.SetIntrinsic(index, text::TextLayout::Measure(visual.text, style2));
        }
        m_Visuals[index] = std::move(visual);
        return index;
    }

    void LayoutDemoLayer::BuildTree(Vec2 viewport) {
        m_Tree.Clear();
        m_Visuals.clear();

        LayoutStyle rootStyle = Row(24.0f, Edges{ 24.0f });
        rootStyle.width = Size::Fill();
        rootStyle.height = Size::Fill();
        rootStyle.align = Align::Start;   // panels hang from the top, not centred in the window
        m_Root = AddNode(rootStyle, LayoutTree::kInvalid);

        // ---- left: a settings card that hugs its content --------------------------------------
        LayoutStyle cardStyle = Column(16.0f, Edges{ 20.0f });
        cardStyle.width = Size::Px(360.0f);
        cardStyle.height = Size::Hug();
        const u32 card = AddNode(cardStyle, m_Root,
                                 DemoVisual{ kPanel, kEdge, 1.0f, Corners{ 14.0f }, "", kText, 15.0f, true });

        AddNode(Column(), card, DemoVisual{ {}, {}, 0.0f, {}, "Appearance", kText, 20.0f });

        auto AddSettingRow = [&](const char* label, const char* value, bool on) {
            LayoutStyle rowStyle = Row(12.0f);
            rowStyle.width = Size::Fill();
            rowStyle.height = Size::Hug();
            const u32 row = AddNode(rowStyle, card);

            AddNode(Column(), row, DemoVisual{ {}, {}, 0.0f, {}, label, kText, 15.0f });

            // A spacer that fills: this is what pushes the control to the right edge, and it is
            // the single most common layout idiom in any real UI.
            LayoutStyle spacerStyle = Column();
            spacerStyle.width = Size::Fill();
            spacerStyle.height = Size::Px(1.0f);
            AddNode(spacerStyle, row);

            if (value) {
                LayoutStyle pill = Row(0.0f, Edges{ 10.0f, 4.0f });
                pill.width = Size::Hug();
                pill.height = Size::Hug();
                const u32 chip = AddNode(pill, row,
                                         DemoVisual{ kSunken, kEdge, 1.0f, Corners{ 6.0f } });
                AddNode(Column(), chip, DemoVisual{ {}, {}, 0.0f, {}, value, kMuted, 13.0f });
            } else {
                LayoutStyle track;
                track.width = Size::Px(40.0f);
                track.height = Size::Px(22.0f);
                const u32 toggle = AddNode(track, row,
                                           DemoVisual{ on ? kAccent : kSunken, kEdge, 1.0f, Corners{ 11.0f } });
                LayoutStyle knob;
                knob.width = Size::Px(16.0f);
                knob.height = Size::Px(16.0f);
                knob.offsetStart = { on ? 21.0f : 3.0f, 3.0f };
                AddNode(knob, toggle, DemoVisual{ { 1.0f, 1.0f, 1.0f, 0.95f }, {}, 0.0f, Corners{ 8.0f } });
            }
        };

        AddSettingRow("Theme", "Dark", false);
        AddSettingRow("Accent colour", "Amber", false);
        AddSettingRow("Reduce motion", nullptr, false);
        AddSettingRow("Snap to grid", nullptr, true);

        // ---- right: a filling column with a wrapping tag row ------------------------------------
        LayoutStyle rightStyle = Column(16.0f);
        rightStyle.width = Size::Fill();
        rightStyle.height = Size::Fill();
        const u32 right = AddNode(rightStyle, m_Root);

        LayoutStyle headerStyle = Row(12.0f, Edges{ 16.0f, 12.0f });
        headerStyle.width = Size::Fill();
        headerStyle.height = Size::Hug();
        const u32 header = AddNode(headerStyle, right,
                                   DemoVisual{ kPanel, kEdge, 1.0f, Corners{ 12.0f } });
        AddNode(Column(), header, DemoVisual{ {}, {}, 0.0f, {}, "Every rect here comes from the layout solver",
                                          kText, 16.0f });

        LayoutStyle tagsStyle = Row(8.0f, Edges{ 16.0f });
        tagsStyle.width = Size::Fill();
        tagsStyle.height = Size::Hug();
        tagsStyle.wrap = true;
        const u32 tags = AddNode(tagsStyle, right,
                                 DemoVisual{ kPanel, kEdge, 1.0f, Corners{ 12.0f } });
        for (const char* tag : { "hug", "fill", "percent", "fixed", "wrap", "gap", "padding",
                                 "align", "justify", "constraints", "aspect ratio", "min/max",
                                 "stack", "absolute", "stretch" }) {
            LayoutStyle chip = Row(0.0f, Edges{ 10.0f, 5.0f });
            chip.width = Size::Hug();
            chip.height = Size::Hug();
            const u32 index = AddNode(chip, tags, DemoVisual{ kSunken, kEdge, 1.0f, Corners{ 12.0f } });
            AddNode(Column(), index, DemoVisual{ {}, {}, 0.0f, {}, tag, kMuted, 13.0f });
        }

        // A footer pinned to the bottom of a filling column, via a Fill spacer above it.
        LayoutStyle spacer = Column();
        spacer.width = Size::Fill();
        spacer.height = Size::Fill();
        AddNode(spacer, right);

        LayoutStyle footerStyle = Row(12.0f, Edges{ 16.0f, 12.0f });
        footerStyle.width = Size::Fill();
        footerStyle.height = Size::Hug();
        footerStyle.justify = Justify::End;
        const u32 footer = AddNode(footerStyle, right,
                                   DemoVisual{ kPanel, kEdge, 1.0f, Corners{ 12.0f } });

        for (auto [label, primary] : { std::pair{ "Cancel", false }, std::pair{ "Save changes", true } }) {
            LayoutStyle button = Row(0.0f, Edges{ 16.0f, 9.0f });
            button.width = Size::Hug();
            button.height = Size::Hug();
            const u32 index = AddNode(button, footer,
                                      DemoVisual{ primary ? kAccent : kSunken,
                                              primary ? Color{ 0, 0, 0, 0 } : kEdge,
                                              primary ? 0.0f : 1.0f, Corners{ 8.0f } });
            AddNode(Column(), index,
                    DemoVisual{ {}, {}, 0.0f, {}, label,
                            primary ? Color{ 0.10f, 0.08f, 0.05f, 1.0f } : kText, 14.0f });
        }

        m_Tree.Compute(m_Root, viewport);
        m_LastViewport = viewport;
    }

    void LayoutDemoLayer::Paint(DrawList& list, u32 node, Vec2 origin) {
        const Rect rect = m_Tree.NodeRect(node);
        const Vec2 pos = origin + rect.pos;
        const DemoVisual& visual = m_Visuals[node];

        if (visual.shadow)
            list.AddShadow(Rect{ pos, rect.size },
                           ShadowSpec{ { 0.0f, 0.0f, 0.0f, 0.5f }, { 0.0f, 8.0f }, 24.0f, 0.0f },
                           visual.corners);

        if (visual.fill.a > 0.0f || visual.borderWidth > 0.0f)
            list.AddRect(Rect{ pos, rect.size }, Paint::Solid(visual.fill), visual.corners,
                         Stroke{ visual.borderWidth, visual.border });

        if (!visual.text.empty()) {
            auto style = m_Font;
            style.size = visual.textSize;
            // `pos` is the TOP-LEFT of the text box, not the baseline: TextLayout already offsets
            // each line's baseline by the font's ascent. Adding it again here pushed every label a
            // full baseline below its own box.
            text::DrawText(list, m_Atlas, visual.text, style, pos, visual.textColor,
                           rect.size.x, text::WrapMode::Word, visual.align);
        }

        for (u32 child : m_Tree.Children(node)) Paint(list, child, pos);
    }

    void LayoutDemoLayer::OnUiRender(gpu::CommandList& cmd) {
        if (!m_Ready) return;

        auto& window = Application::Get().GetWindow();
        const Vec2 viewport{ static_cast<f32>(window.Width()), static_cast<f32>(window.Height()) };

        // Relayout only when the box changed. Everything else is idle.
        if (viewport != m_LastViewport || m_Root == LayoutTree::kInvalid) BuildTree(viewport);

        m_List.Reset();
        Paint(m_List, m_Root, { 0.0f, 0.0f });

        m_Renderer.NewFrame();
        m_Renderer.Render(cmd, m_List, viewport);
    }

    void LayoutDemoLayer::OnEvent(Event& e) {
        if (e.type == EventType::KeyPressed && e.key.code == Key::Escape)
            Application::Get().Close();
    }

}
