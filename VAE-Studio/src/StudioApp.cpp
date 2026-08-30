#include "vae/app/ImGuiLayer.h"
#include "vae/base/Version.h"
#include "vae/core/Application.h"
#include "vae/core/EntryPoint.h"

#include "Convert.h"
#include "Export.h"
#include "Selftest.h"
#include "StudioLayer.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace vae {

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
            if (Spec().args.Has("--export")) {
                const CommandLineArgs& args = Spec().args;
                std::filesystem::path project, out;
                for (int i = 1; i < args.count; ++i) {
                    const std::string_view a = args[i];
                    if (a.starts_with("--")) continue;
                    if (project.empty()) project = a; else if (out.empty()) out = a;
                }
                PushLayer(CreateScope<ExportLayer>(std::move(project), std::move(out)));
                return;
            }
            if (Spec().args.Has("--convert")) {
                const CommandLineArgs& args = Spec().args;
                std::filesystem::path in, out;
                for (int i = 1; i < args.count; ++i) {
                    const std::string_view a = args[i];
                    // A flag that takes a value eats the token after it. Without this,
                    // `--convert --bench 20 file.vae` reads "20" as the file to convert.
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

            PushLayer(CreateScope<StudioLayer>());
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
        spec.enableImGui    = true;   // the editor's panels; nothing else in VAE asks for chrome

        // --selftest is not a hidden window: it is no window and no device at all. Every check it
        // runs is about the document, the layout and the gestures, none of which need a GPU, and a
        // verification pass that needs one cannot run where it is most wanted.
        if (args.Has("--selftest") || args.Has("--convert") || args.Has("--export")
            || args.Has("--version")) {
            spec.createWindow = false;
            spec.createDevice = false;
            spec.enableImGui  = false;
            return new StudioApp(std::move(spec));
        }

        return new StudioApp(std::move(spec));
    }

}
