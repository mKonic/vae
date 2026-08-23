#include "vae/app/ImGuiLayer.h"
#include "vae/base/Version.h"
#include "vae/core/Application.h"
#include "vae/core/EntryPoint.h"
#include "vae/draw/Renderer.h"
#include "vae/text/FontDB.h"
#include "vae/text/TextDraw.h"

#include "Convert.h"
#include "LayoutDemo.h"
#include "Selftest.h"
#include "StudioLayer.h"
#include "WidgetGallery.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace vae {

    // P3 verification scene: every primitive the draw layer can produce, in one frame, so a
    // screenshot is a regression test for all of them at once. Replaced by the real editor in P8.
    class PrimitiveZooLayer final : public Layer {
    public:
        PrimitiveZooLayer() : Layer("PrimitiveZoo") {}

        void OnAttach() override {
            auto& app = Application::Get();
            if (!app.HasDevice()) return;
            auto& device = app.GetDevice();

            const gpu::Format format = device.GetSwapchain() ? device.GetSwapchain()->ColorFormat()
                                                             : gpu::Format::BGRA8_UNORM;
            if (!m_Renderer.Init(device, format)) {
                VAE_ERROR("draw renderer failed to initialise");
                return;
            }
            m_Ready = true;

            // A checkerboard, drawn nearest-filtered so scaling artefacts would be obvious.
            constexpr u32 kSize = 16;
            std::vector<u32> pixels(kSize * kSize);
            for (u32 y = 0; y < kSize; ++y)
                for (u32 x = 0; x < kSize; ++x)
                    pixels[y * kSize + x] = ((x / 4 + y / 4) % 2) ? 0xFF3C3C46u : 0xFFE8E6F0u;

            gpu::TextureDesc desc;
            desc.width = desc.height = kSize;
            desc.format = gpu::Format::RGBA8_UNORM;
            desc.minFilter = desc.magFilter = gpu::Filter::Nearest;
            desc.debugName = "zoo.checker";
            m_Checker = device.CreateTexture(desc);
            m_Checker->Upload(pixels.data(), pixels.size() * sizeof(u32));

            auto& fonts = text::FontDB::Get();
            fonts.LoadDefaults();
            m_Body     = fonts.Style({ "", text::FontWeight::Regular, text::FontSlant::Normal, 15.0f });
            m_Heading  = fonts.Style({ "", text::FontWeight::Medium,  text::FontSlant::Normal, 30.0f });
            m_Small    = fonts.Style({ "", text::FontWeight::Regular, text::FontSlant::Normal, 11.0f });
            m_Atlas.Init(device);
        }

        void OnDetach() override {
            if (!m_Ready) return;
            m_Atlas.Shutdown();
            m_Renderer.Shutdown();
        }

        void OnUiRender(gpu::CommandList& cmd) override {
            if (!m_Ready) return;

            auto& window = Application::Get().GetWindow();
            const Vec2 viewport{ static_cast<f32>(window.Width()), static_cast<f32>(window.Height()) };

            m_List.Reset();
            BuildScene(m_List, viewport);

            // Deliberately a SECOND list rendered in the same frame. Ladle's 2D renderer refilled
            // one buffer from offset zero per scene, so a second BeginScene silently erased the
            // first before the GPU read it. If that regressed here, the zoo above would vanish.
            m_Overlay.Reset();
            BuildOverlay(m_Overlay, viewport);

            m_Renderer.NewFrame();
            m_Renderer.Render(cmd, m_List, viewport);
            m_Renderer.Render(cmd, m_Overlay, viewport);
        }

        void OnEvent(Event& e) override {
            if (e.type == EventType::KeyPressed && e.key.code == Key::Escape)
                Application::Get().Close();
        }

    private:
        static constexpr Color kInk{ 0.93f, 0.62f, 0.27f, 1.0f };   // amber accent
        static constexpr Color kPanel{ 0.13f, 0.14f, 0.17f, 1.0f };
        static constexpr Color kEdge{ 0.28f, 0.30f, 0.36f, 1.0f };

        void BuildScene(draw::DrawList& list, Vec2 viewport) {
            using namespace draw;

            // 1. Shadowed card with a border and asymmetric corners.
            const Rect card{ { 60.0f, 60.0f }, { 320.0f, 180.0f } };
            const Corners cardCorners{ 24.0f, 8.0f, 24.0f, 8.0f };
            list.AddShadow(card, ShadowSpec{ { 0.0f, 0.0f, 0.0f, 0.55f }, { 0.0f, 10.0f }, 28.0f, 0.0f },
                           cardCorners);
            list.AddRect(card, Paint::Solid(kPanel), cardCorners, Stroke{ 2.0f, kEdge });

            // 2. Linear and radial gradients.
            list.AddRect(Rect{ { 92.0f, 92.0f }, { 120.0f, 116.0f } },
                         Paint::Linear(kInk, { 0.35f, 0.72f, 0.94f, 1.0f },
                                       { 0.0f, 0.0f }, { 1.0f, 1.0f }),
                         Corners{ 12.0f });
            list.AddRect(Rect{ { 228.0f, 92.0f }, { 120.0f, 116.0f } },
                         Paint::Radial({ 0.98f, 0.85f, 0.55f, 1.0f }, { 0.55f, 0.25f, 0.65f, 1.0f }),
                         Corners{ 58.0f });

            // 3. Corner-radius ramp: 0 through fully round, proving the per-corner SDF.
            for (int i = 0; i < 6; ++i) {
                const f32 x = 60.0f + static_cast<f32>(i) * 68.0f;
                list.AddRect(Rect{ { x, 280.0f }, { 56.0f, 56.0f } },
                             Paint::Solid({ 0.35f + 0.1f * i, 0.45f, 0.85f - 0.08f * i, 1.0f }),
                             Corners{ static_cast<f32>(i) * 5.6f });
            }

            // 4. Border widths, including one thick enough to meet in the middle.
            for (int i = 0; i < 4; ++i) {
                const f32 x = 60.0f + static_cast<f32>(i) * 68.0f;
                list.AddRect(Rect{ { x, 360.0f }, { 56.0f, 56.0f } },
                             Paint::Solid({ 0.10f, 0.11f, 0.14f, 1.0f }), Corners{ 10.0f },
                             Stroke{ 1.0f + static_cast<f32>(i) * 4.0f, kInk });
            }

            // 5. Image fill, and the same image clipped by a rounded rect. The clip is what proves
            //    device-space clipping is independent of the clipped shape's own geometry.
            list.AddRect(Rect{ { 440.0f, 60.0f }, { 140.0f, 140.0f } },
                         Paint::Image(m_Checker), Corners{ 8.0f });

            list.PushClip(Rect{ { 600.0f, 60.0f }, { 140.0f, 140.0f } }, Corners{ 70.0f });
            list.AddRect(Rect{ { 590.0f, 50.0f }, { 160.0f, 160.0f } }, Paint::Image(m_Checker));
            list.AddRect(Rect{ { 590.0f, 150.0f }, { 160.0f, 60.0f } },
                         Paint::Solid({ kInk.r, kInk.g, kInk.b, 0.75f }));
            list.PopClip();

            // 6. Nested clips: the inner rect is cut by both.
            list.PushClip(Rect{ { 440.0f, 240.0f }, { 300.0f, 120.0f } });
            list.PushClip(Rect{ { 500.0f, 200.0f }, { 120.0f, 300.0f } });
            list.AddRect(Rect{ { 400.0f, 180.0f }, { 400.0f, 400.0f } },
                         Paint::Solid({ 0.45f, 0.85f, 0.60f, 1.0f }));
            list.PopClip();
            list.PopClip();

            // 7. Transform stack: the same card again at half scale, proving radii, border widths
            //    and antialiasing all scale with it rather than staying in stale units.
            list.PushTransform({ 0.5f, 0.5f }, { 1000.0f, 560.0f });
            list.AddShadow(card, ShadowSpec{ { 0.0f, 0.0f, 0.0f, 0.55f }, { 0.0f, 10.0f }, 28.0f, 0.0f },
                           cardCorners);
            list.AddRect(card, Paint::Solid(kPanel), cardCorners, Stroke{ 2.0f, kEdge });
            list.AddRect(Rect{ { 92.0f, 92.0f }, { 256.0f, 116.0f } },
                         Paint::Linear(kInk, { 0.35f, 0.72f, 0.94f, 1.0f },
                                       { 0.0f, 0.0f }, { 1.0f, 0.0f }),
                         Corners{ 12.0f });
            list.PopTransform();

            // 8. Text. Same instanced pipeline as everything above — a glyph is a fill kind, not a
            //    second renderer — so this batches with the boxes around it.
            using namespace vae::text;
            const Color ink{ 0.92f, 0.93f, 0.96f, 1.0f };
            const Color dim{ 0.62f, 0.65f, 0.72f, 1.0f };

            DrawText(list, m_Atlas, "Virtual App Engine", m_Heading, { 800.0f, 70.0f }, ink);
            DrawText(list, m_Atlas, "Instanced SDF primitives, one draw call per batch.",
                     m_Body, { 800.0f, 112.0f }, dim);

            // Wrapping paragraph inside a visible box, so the wrap width is checkable by eye.
            const Rect column{ { 800.0f, 150.0f }, { 380.0f, 150.0f } };
            list.AddRect(column, Paint::Solid({ 1.0f, 1.0f, 1.0f, 0.03f }), Corners{ 8.0f });
            DrawText(list, m_Atlas,
                     "Text measurement runs with no GPU present, which is what lets layout be unit "
                     "tested headlessly. Wrapping breaks at spaces, and a word longer than the "
                     "column breaks mid-word rather than running off the edge.",
                     m_Body, { column.Left() + 12.0f, column.Top() + 14.0f }, dim,
                     column.size.x - 24.0f);

            // Alignment, against a rule so the edges are verifiable.
            const f32 alignLeft = 800.0f, alignWidth = 380.0f;
            list.AddLine({ alignLeft, 318.0f }, { alignLeft + alignWidth, 318.0f }, 1.0f, kEdge);
            DrawText(list, m_Atlas, "left", m_Small, { alignLeft, 330.0f }, dim, alignWidth,
                     WrapMode::Word, TextAlign::Left);
            DrawText(list, m_Atlas, "centre", m_Small, { alignLeft, 348.0f }, dim, alignWidth,
                     WrapMode::Word, TextAlign::Center);
            DrawText(list, m_Atlas, "right", m_Small, { alignLeft, 366.0f }, dim, alignWidth,
                     WrapMode::Word, TextAlign::Right);

            // A size ramp: the atlas rasterizes per size on demand, so these are all crisp.
            f32 y = 400.0f;
            for (f32 size : { 10.0f, 12.0f, 14.0f, 18.0f, 24.0f }) {
                auto style = m_Body;
                style.size = size;
                DrawText(list, m_Atlas, "The quick brown fox 0123", style, { 800.0f, y }, ink);
                y += size + 10.0f;
            }

            // Labels on the shapes above, proving text composes with the rest of the scene.
            DrawText(list, m_Atlas, "corner radii", m_Small, { 60.0f, 262.0f }, dim);
            DrawText(list, m_Atlas, "border widths", m_Small, { 60.0f, 342.0f }, dim);
            DrawText(list, m_Atlas, "clipped", m_Small, { 500.0f, 222.0f }, dim);

            // 9. Dividers, and a translucent overlay proving straight-alpha blending.
            list.AddLine({ 60.0f, 440.0f }, { viewport.x - 60.0f, 440.0f }, 1.0f, kEdge);
            list.AddRect(Rect{ { 60.0f, 470.0f }, { 200.0f, 60.0f } },
                         Paint::Solid({ 1.0f, 1.0f, 1.0f, 0.15f }), Corners{ 8.0f });
            list.AddRect(Rect{ { 140.0f, 490.0f }, { 200.0f, 60.0f } },
                         Paint::Solid({ kInk.r, kInk.g, kInk.b, 0.5f }), Corners{ 8.0f });
        }

        void BuildOverlay(draw::DrawList& list, Vec2 viewport) {
            using namespace draw;
            const Rect badge{ { 60.0f, viewport.y - 110.0f }, { 190.0f, 44.0f } };
            list.AddShadow(badge, ShadowSpec{ { 0.0f, 0.0f, 0.0f, 0.6f }, { 0.0f, 6.0f }, 18.0f, 0.0f },
                           Corners{ 22.0f });
            list.AddRect(badge, Paint::Linear({ 0.20f, 0.22f, 0.28f, 1.0f },
                                              { 0.12f, 0.13f, 0.16f, 1.0f },
                                              { 0.0f, 0.0f }, { 0.0f, 1.0f }),
                         Corners{ 22.0f }, Stroke{ 1.0f, kEdge });
            list.AddRect(Rect{ { badge.Left() + 14.0f, badge.Top() + 14.0f }, { 16.0f, 16.0f } },
                         Paint::Solid(kInk), Corners{ 8.0f });
        }

        draw::Renderer      m_Renderer;
        text::GlyphAtlas    m_Atlas;
        text::TextStyle     m_Body, m_Heading, m_Small;
        draw::DrawList      m_List;
        draw::DrawList      m_Overlay;
        Ref<gpu::Texture>   m_Checker;
        bool                m_Ready = false;
    };

    class FrameLimitLayer final : public Layer {
    public:
        explicit FrameLimitLayer(u64 frames) : Layer("FrameLimit"), m_Frames(frames) {}
        void OnUpdate(Timestep) override {
            Application::Get().RequestFrame();
            if (Application::Get().FrameCount() >= m_Frames) {
                VAE_INFO("frame limit reached ({} frames) — exiting", m_Frames);
                Application::Get().Close();
            }
        }
    private:
        u64 m_Frames;
    };

    class StudioApp final : public Application {
    public:
        explicit StudioApp(AppSpec spec) : Application(std::move(spec)) {
            if (Spec().args.Has("--version")) {
                std::printf("VAE Studio %s\n", Version::String().c_str());
                Application::Get().Close();
                return;
            }
            if (Spec().args.Has("--selftest")) {
                PushLayer(CreateScope<SelftestLayer>());
                return;
            }
            if (Spec().args.Has("--convert")) {
                const CommandLineArgs& args = Spec().args;
                std::filesystem::path in, out;
                for (int i = 1; i < args.count; ++i) {
                    const std::string_view a = args[i];
                    // A flag that takes a value eats the token after it. Without this,
                    // `--convert --bench 20 file.vaescreen` reads "20" as the file to convert.
                    if (a == "--bench") { ++i; continue; }
                    if (a.starts_with("--")) continue;
                    if (in.empty()) in = a; else if (out.empty()) out = a;
                }
                int bench = 0;
                if (const auto n = args.Value("--bench")) bench = std::atoi(std::string(*n).c_str());
                PushLayer(CreateScope<ConvertLayer>(std::move(in), std::move(out),
                                                    args.Has("--check"), bench));
                return;
            }

            // VAE_SCENE picks the verification scene: `zoo` is the P3/P4 primitive zoo, `layout`
            // the P5 solver demo. The default is the P7 widget gallery, which exercises the most.
            const char* scene = std::getenv("VAE_SCENE");
            const std::string_view name = scene ? std::string_view(scene) : std::string_view("studio");
            if (name == "zoo")          PushLayer(CreateScope<PrimitiveZooLayer>());
            else if (name == "layout")  PushLayer(CreateScope<LayoutDemoLayer>());
            else if (name == "widgets") PushLayer(CreateScope<WidgetGalleryLayer>());
            else                        PushLayer(CreateScope<StudioLayer>());
            if (const char* frames = std::getenv("VAE_FRAMES"))
                PushOverlay(CreateScope<FrameLimitLayer>(std::strtoull(frames, nullptr, 10)));
        }
    };

    Application* CreateApplication(CommandLineArgs args) {
        // The editor is the only thing that ships ImGui. Naming the factory here is what pulls the
        // toolkit into this binary and leaves it out of the player and of every exported app.
        Application::SetChromeFactory(&app::MakeImGuiLayer);

        AppSpec spec;
        spec.name           = "VAE Studio";
        spec.args           = args;
        spec.window.title   = "VAE Studio";
        spec.window.wmClass = "VAE";

        // --selftest is not a hidden window: it is no window and no device at all. Every check it
        // runs is about the document, the layout and the gestures, none of which need a GPU, and a
        // verification pass that needs one cannot run where it is most wanted.
        if (args.Has("--selftest") || args.Has("--convert") || args.Has("--version")) {
            spec.createWindow = false;
            spec.createDevice = false;
            spec.enableImGui  = false;
            return new StudioApp(std::move(spec));
        }

        // The verification scenes render the document directly; only the editor wants chrome.
        const char* scene = std::getenv("VAE_SCENE");
        spec.enableImGui = !scene || std::string_view(scene) == "studio";
        return new StudioApp(std::move(spec));
    }

}
